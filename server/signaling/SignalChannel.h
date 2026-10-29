#pragma once
#include "BoundedChannel.h"

namespace server::signaling
{
using SignalResult = utils::ChannelResult;
using SignalBudget = utils::ChannelBudget;

// 信令通道只保留首响先于通知的规则；排队、预算和异步完成由公共组件实现。
template<class Message>
class SignalChannel
{
    using Channel = utils::BoundedChannel<Message>;
public:
    using Post = typename Channel::Post;
    using Consume = typename Channel::Consume;
    using Complete = typename Channel::Complete;
    using AsyncConsume = typename Channel::AsyncConsume;

    SignalChannel(Post post, Consume consume, std::shared_ptr<SignalBudget> budget,
                  std::size_t max_bytes = 4 * 1024 * 1024, std::size_t max_messages = 64,
                  bool gate_notifications = false)
        : channel_(std::move(post), std::move(consume), std::move(budget),
                   {max_bytes, max_messages}, gate_notifications) {}

    SignalChannel(Post post, AsyncConsume consume, std::shared_ptr<SignalBudget> budget,
                  std::size_t max_bytes = 4 * 1024 * 1024, std::size_t max_messages = 64)
        : channel_(std::move(post), std::move(consume), std::move(budget), {max_bytes, max_messages}) {}

    SignalResult Send(Message message, std::size_t bytes, bool notification = false, bool initial = false)
    {
        using Delivery = utils::ChannelDelivery;
        const auto delivery = initial ? Delivery::OpenGate : notification ? Delivery::Deferred : Delivery::Ready;
        return channel_.Send(std::move(message), bytes, delivery);
    }
    void Close() { channel_.Close(); }
    void Cancel() { channel_.Cancel(); }
    SignalResult Status() const { return channel_.Status(); }
    void ResetNotificationGate() { channel_.ResetGate(); }

private:
    Channel channel_;
};
}
