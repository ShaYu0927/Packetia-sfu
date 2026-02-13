#ifndef _SOURCEBASE_H__
#define _SOURCEBASE_H__

#include <cstddef>
#include <functional>
#include <memory>

#include "CallbackEmitter.h"

struct ISourceBase
{
    virtual ~ISourceBase() = default;
    virtual size_t subscriberCount() const = 0;
};

template<class T>
struct ISource : public ISourceBase
{
public:
    using Item = T;
    using Callback = std::function<void(const Item&)>;

    virtual std::shared_ptr<ISubscription> subscribe(Callback cb) = 0;

protected:
    virtual void publishImpl(const Item& item) = 0;
    virtual void publishImpl(Item&& item) = 0;

};


template<class T>
class SourceCOW final : public ISource<T>
{
public:
    using Item = T;
    using Callback = typename ISource<T>::Callback;

    std::shared_ptr<ISubscription> subscribe(Callback cb) override
    {
        return sig_.subscribe(std::move(cb));
    }

    size_t subscriberCount() const override
    {
        return sig_.size();
    }

    void publish(const Item& v) 
    {
        publishImpl(v);
    }
    void publish(Item&& v) 
    {
        publishImpl(std::move(v));
    }

protected:
    void publishImpl(const Item& v) override 
    {
        sig_.emit(v);
    }
    void publishImpl(Item&& v) override 
    {
        sig_.emit(std::move(v));
    }

private:
    SignalCOW<Item> sig_;
};



#endif