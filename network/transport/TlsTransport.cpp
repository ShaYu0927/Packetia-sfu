#include "TlsTransport.h"
#include "../../Common/memory/SharedBuffer.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <deque>
#include <limits>

namespace network::transport
{

struct TlsTransport::Impl
{
    static constexpr size_t kRecordBytes = 16 * 1024;
    static constexpr size_t kDriveBudget = 32;
    struct PendingWrite { common::SharedBuffer bytes; size_t offset = 0; };

    TcpConnection::Ptr connection;
    std::shared_ptr<TlsContext> context;
    Options options;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
    BIO* input = nullptr;
    BIO* output = nullptr;
    Callbacks callbacks;
    std::weak_ptr<TlsTransport> owner;
    std::atomic<State> state{State::Created};
    std::atomic<Error> error{Error::None};
    TimeId timer = 0;
    std::deque<PendingWrite> writes;
    size_t queued_plaintext = 0;
    bool driving = false;
    bool continuation_pending = false;
    bool shutdown_started = false;

    TaskScheduler* Scheduler() const { return connection->GetTaskScheduler(); }
    bool Terminal() const { return state == State::Closed || state == State::Failed; }

    void CancelTimer()
    {
        if (timer) { Scheduler()->RemoveTimer(timer); timer = 0; }
    }

    void Finish(Error reason)
    {
        if (Terminal()) return;
        error = reason;
        state = reason == Error::None || reason == Error::LocalClose ? State::Closed : State::Failed;
        CancelTimer();
        writes.clear();
        queued_plaintext = 0;
        connection->Disconnect();
        auto closed = callbacks.on_closed;
        if (closed) closed(reason);
    }

    bool FlushCiphertext()
    {
        if (Terminal()) return false;
        if (BIO_ctrl_pending(output) > options.max_ciphertext_bytes) {
            Finish(Error::BufferLimit);
            return false;
        }
        std::array<char, kRecordBytes> discarded{};
        while (BIO_ctrl_pending(output) != 0) {
            char* bytes = nullptr;
            const auto available = BIO_get_mem_data(output, &bytes);
            if (available <= 0) { Finish(Error::IoError); return false; }
            const auto size = std::min({static_cast<size_t>(available), kRecordBytes,
                                       connection->SendCapacityBytes()});
            if (size == 0) { Finish(Error::IoError); return false; }
            const auto result = connection->Send(bytes, static_cast<uint32_t>(size));
            if (result == TcpConnection::SendResult::QueueFull) return true;
            if (result != TcpConnection::SendResult::Queued) { Finish(Error::IoError); return false; }
            // Consume only after TCP accepts a copy. Backpressure leaves all
            // remaining ciphertext in the BIO, preserving exact byte order.
            if (BIO_read(output, discarded.data(), static_cast<int>(size)) != static_cast<int>(size)) {
                Finish(Error::IoError);
                return false;
            }
        }
        return true;
    }

    void Continue()
    {
        if (continuation_pending || Terminal()) return;
        continuation_pending = true;
        if (!Scheduler()->Post([weak = owner] {
            if (auto self = weak.lock()) {
                self->impl_->continuation_pending = false;
                self->impl_->Drive();
            }
        })) {
            continuation_pending = false;
            Finish(Error::IoError);
        }
    }

    void PeerShutdown()
    {
        state = State::Closing;
        writes.clear();
        queued_plaintext = 0;
        CancelTimer();
        timer = Scheduler()->AddTimer([weak = owner] {
            if (auto self = weak.lock()) self->impl_->Finish(Error::IoError);
            return false;
        }, options.shutdown_timeout_ms);
    }

    void Drive()
    {
        if (driving || Terminal() || !ssl) return;
        driving = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{driving};
        if (!FlushCiphertext()) return;

        if (state == State::Handshaking) {
            ERR_clear_error();
            const int result = SSL_do_handshake(ssl.get());
            const int code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
            if (result != 1 && code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
                Finish(Error::ProtocolError);
                return;
            }
            if (!FlushCiphertext()) return;
            if (result != 1) return;
            CancelTimer();
            state = State::Open;
            auto ready = callbacks.on_ready;
            if (ready) ready();
            if (Terminal()) return;
        }

        size_t reads = 0;
        std::array<uint8_t, kRecordBytes> plaintext{};
        while (state == State::Open && reads < kDriveBudget) {
            size_t size = 0;
            ERR_clear_error();
            const int result = SSL_read_ex(ssl.get(), plaintext.data(), plaintext.size(), &size);
            const int code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
            if (!FlushCiphertext()) return;
            if (result == 1) {
                ++reads;
                auto receive = callbacks.on_bytes;
                if (receive) receive(plaintext.data(), size);
                if (Terminal()) return;
                continue;
            }
            if (code == SSL_ERROR_ZERO_RETURN) { PeerShutdown(); break; }
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) break;
            Finish(Error::ProtocolError);
            return;
        }

        size_t sent = 0;
        while (state == State::Open && !writes.empty() && sent < kDriveBudget) {
            if (!FlushCiphertext()) return;
            if (BIO_ctrl_pending(output) != 0) break;
            auto& pending = writes.front();
            const auto size = std::min(kRecordBytes, pending.bytes.Size() - pending.offset);
            size_t consumed = 0;
            ERR_clear_error();
            const int result = SSL_write_ex(ssl.get(), pending.bytes.Data() + pending.offset, size, &consumed);
            const int code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
            if (result != 1 && code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
                Finish(Error::ProtocolError);
                return;
            }
            if (result == 1) {
                pending.offset += consumed;
                queued_plaintext -= consumed;
                if (pending.offset == pending.bytes.Size()) writes.pop_front();
                ++sent;
            }
            if (!FlushCiphertext()) return;
            if (result != 1) break;
        }

        if (state == State::Closing) {
            if (!shutdown_started) {
                shutdown_started = true;
                ERR_clear_error();
                const int result = SSL_shutdown(ssl.get());
                const int code = result < 0 ? SSL_get_error(ssl.get(), result) : SSL_ERROR_NONE;
                if (result < 0 && code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
                    Finish(Error::ProtocolError);
                    return;
                }
            }
            if (!FlushCiphertext()) return;
            if (BIO_ctrl_pending(output) == 0 && connection->QueuedSendBytes() == 0)
                Finish(Error::None);
            return;
        }
        if ((reads == kDriveBudget && (SSL_pending(ssl.get()) > 0 || BIO_ctrl_pending(input) > 0)) ||
            (sent == kDriveBudget && !writes.empty() && BIO_ctrl_pending(output) == 0))
            Continue();
    }

    void Receive(const uint8_t* data, size_t size)
    {
        if (Terminal() || state == State::Closing) return;
        const size_t buffered = BIO_ctrl_pending(input);
        if (size > options.max_input_bytes - std::min(options.max_input_bytes, buffered) ||
            size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            Finish(Error::BufferLimit);
            return;
        }
        if (BIO_write(input, data, static_cast<int>(size)) != static_cast<int>(size)) {
            Finish(Error::IoError);
            return;
        }
        Drive();
    }
};

TlsTransport::TlsTransport(TcpConnection::Ptr connection, std::shared_ptr<TlsContext> context)
    : TlsTransport(std::move(connection), std::move(context), Options{}) {}

TlsTransport::TlsTransport(TcpConnection::Ptr connection, std::shared_ptr<TlsContext> context,
                           Options options) : impl_(std::make_unique<Impl>())
{
    impl_->connection = std::move(connection);
    impl_->context = std::move(context);
    impl_->options = options;
}

TlsTransport::~TlsTransport()
{
    if (!impl_->connection) return;
    impl_->Scheduler()->Invoke([this] {
        impl_->CancelTimer();
        if (impl_->state == State::Created) return;
        impl_->connection->SetBytesCallback({});
        impl_->connection->SetCloseCallback(TcpConnection::CloseCallback{});
        impl_->connection->SetWriteCompleteCallback({});
        impl_->connection->Disconnect();
    });
}

bool TlsTransport::Start(Callbacks callbacks)
{
    if (!impl_->connection || !impl_->context || weak_from_this().expired()) return false;
    auto self = shared_from_this();
    bool started = false;
    impl_->Scheduler()->Invoke([&, self] {
        auto& p = *impl_;
        if (p.state != State::Created || p.connection->IsClosed() || p.Scheduler()->IsStopped() ||
            !p.options.handshake_timeout_ms || !p.options.shutdown_timeout_ms ||
            !p.options.max_plaintext_bytes || !p.options.max_input_bytes || !p.options.max_ciphertext_bytes) return;
        p.ssl.reset(SSL_new(p.context->context_));
        BIO* input = BIO_new(BIO_s_mem());
        BIO* output = BIO_new(BIO_s_mem());
        if (!p.ssl || !input || !output) {
            BIO_free(input);
            BIO_free(output);
            p.ssl.reset();
            return;
        }
        BIO_set_mem_eof_return(input, -1);
        SSL_set_bio(p.ssl.get(), input, output);
        SSL_set_accept_state(p.ssl.get());
        p.input = input;
        p.output = output;
        p.owner = self;
        p.callbacks = std::move(callbacks);
        p.state = State::Handshaking;
        p.connection->SetBytesCallback([weak = p.owner](auto, const uint8_t* bytes, size_t size) {
            if (auto transport = weak.lock()) transport->impl_->Receive(bytes, size);
        });
        p.connection->SetWriteCompleteCallback([weak = p.owner](auto) {
            if (auto transport = weak.lock()) transport->impl_->Drive();
        });
        p.connection->SetCloseCallback(TcpConnection::CloseCallback([weak = p.owner](auto) {
            if (auto transport = weak.lock()) {
                auto& state = *transport->impl_;
                state.Finish(state.state == State::Closing ? Error::None : Error::TcpClosed);
            }
        }));
        p.timer = p.Scheduler()->AddTimer([weak = p.owner] {
            if (auto transport = weak.lock()) transport->impl_->Finish(Error::HandshakeTimeout);
            return false;
        }, p.options.handshake_timeout_ms);
        p.connection->Start();
        p.Drive();
        started = !p.Terminal();
    });
    return started;
}

TlsTransport::SendResult TlsTransport::Send(const uint8_t* data, size_t size)
{
    if (!data || !size || !impl_->connection) return SendResult::Failed;
    auto self = shared_from_this();
    SendResult result = SendResult::Closed;
    impl_->Scheduler()->Invoke([&, self] {
        auto& p = *impl_;
        if (p.Terminal() || p.state == State::Closing || p.Scheduler()->IsStopped()) return;
        if (p.state != State::Open) { result = SendResult::NotWritable; return; }
        if (size > p.options.max_plaintext_bytes - p.queued_plaintext) {
            result = SendResult::NotWritable;
            return;
        }
        auto bytes = common::SharedBuffer::TryCopy(data, size);
        if (bytes.Empty()) { result = SendResult::Failed; return; }
        try { p.writes.push_back({std::move(bytes), 0}); }
        catch (const std::bad_alloc&) { result = SendResult::Failed; return; }
        p.queued_plaintext += size;
        result = SendResult::Queued;
        p.Drive();
    });
    return result;
}

void TlsTransport::Close()
{
    if (!impl_->connection) return;
    auto self = shared_from_this();
    impl_->Scheduler()->Invoke([this, self] { impl_->Finish(Error::LocalClose); });
}

TlsTransport::State TlsTransport::GetState() const noexcept { return impl_->state.load(); }
TlsTransport::Error TlsTransport::LastError() const noexcept { return impl_->error.load(); }

} // namespace network::transport
