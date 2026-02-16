#ifndef _TCPSESSION_H_
#define _TCPSESSION_H_

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cstdint>

#include "TcpConnection.h"

namespace itcp_sess {

/* -------- ObserverList-------- */
template <typename Obs>
class ObserverList {
public:
    void Add(const std::shared_ptr<Obs>& o) 
    {
        if (!o) return;
        observers_.push_back(o);
    }

    void Remove(const std::shared_ptr<Obs>& o) 
    {
        observers_.erase(
            std::remove_if(observers_.begin(), observers_.end(),
                [&](const std::weak_ptr<Obs>& w)
                {
                    auto sp = w.lock();
                    return !sp || sp == o;
                }),
            observers_.end());
    }

    template <typename Fn>
    void Notify(Fn&& fn) 
    {
        std::vector<std::shared_ptr<Obs>> snap;
        snap.reserve(observers_.size());
        for (auto& w : observers_) 
        {
            if (auto sp = w.lock()) snap.push_back(std::move(sp));
        }
        observers_.erase(
            std::remove_if(observers_.begin(), observers_.end(),
                [](const std::weak_ptr<Obs>& w) { return w.expired(); }),
            observers_.end());

        for (auto& o : snap) fn(*o);
    }

private:
    std::vector<std::weak_ptr<Obs>> observers_;
};


template<typename Msg>
class ICodec 
{
public:
    virtual ~ICodec() = default;
    virtual void Feed(const uint8_t* data, size_t len, std::vector<Msg>& out) = 0;
    virtual void Encode(const Msg& msg, std::vector<uint8_t>& out) = 0;
};

/* -------- Session observer (message-level) -------- */
template<typename Msg>
class ISessionObserver
{
public:
    virtual ~ISessionObserver() = default;
    virtual void OnMessage(const Msg& msg) = 0;
    virtual void OnSessionClosed(int reason) {}
};

/* -------- Connection observer (bytes-level) -------- */
class IConnectionObserver 
{
public:
    virtual ~IConnectionObserver() = default;
    virtual void OnBytes(const uint8_t* data, size_t len) = 0;
    virtual void OnConnClosed(int reason) {}
    virtual void OnConnError(int err) {}
};

// -------- TcpSession --------
template<typename Msg>
class TcpSession : public IConnectionObserver,
                   public std::enable_shared_from_this<TcpSession<Msg>>
{
public:
    using Ptr = std::shared_ptr<TcpSession<Msg>>;

    TcpSession(std::shared_ptr<TcpConnection> conn,
               std::unique_ptr<ICodec<Msg>> codec)
        : conn_(std::move(conn)), codec_(std::move(codec)) {}

    void Start() 
    {
        std::weak_ptr<TcpSession<Msg>> weak = this->shared_from_this();

        conn_->SetBytesCallback([weak](const uint8_t* data, size_t len) {
            if (auto self = weak.lock()) self->OnBytes(data, len);
        });

        conn_->SetCloseCallback([weak](int reason) {
            if (auto self = weak.lock()) self->OnConnClosed(reason);
        });

        conn_->Start();
    }

    void AddObserver(const std::shared_ptr<ISessionObserver<Msg>>& obs) 
    {
        sess_observers_.Add(obs);
    }
    void RemoveObserver(const std::shared_ptr<ISessionObserver<Msg>>& obs) 
    {
        sess_observers_.Remove(obs);
    }

    void Send(const Msg& msg) 
    {
        std::vector<uint8_t> out;
        codec_->Encode(msg, out);
        if (!out.empty()) 
        {
            conn_->Send(reinterpret_cast<const char*>(out.data()),
                        static_cast<uint32_t>(out.size()));
        }
    }

    void Close(int reason = 0) 
    {
        conn_->Disconnect();
        (void)reason;
    }

    /* -------- IConnectionObserver -------- */
    void OnBytes(const uint8_t* data, size_t len) override 
    {
        std::vector<Msg> msgs;
        codec_->Feed(data, len, msgs);
        for (auto& m : msgs) 
        {
            sess_observers_.Notify([&](ISessionObserver<Msg>& o)
            {
                o.OnMessage(m);
            });
        }
    }

    void OnConnClosed(int reason) override 
    {
        sess_observers_.Notify([&](ISessionObserver<Msg>& o)
        {
            o.OnSessionClosed(reason);
        });
    }

private:
    std::shared_ptr<TcpConnection> conn_;
    std::unique_ptr<ICodec<Msg>> codec_;
    ObserverList<ISessionObserver<Msg>> sess_observers_;
};

} // namespace itcp_sess

#endif // _TCPSESSION_H_
