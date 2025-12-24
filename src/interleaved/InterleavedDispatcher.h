#ifndef _INTERLEAVEDDISPATCHER_H_
#define _INTERLEAVEDDISPATCHER_H_

#include <cstdint>
#include <utility>
#include <memory>


class InterleavedHandler 
{
public:
    template<typename T>
    InterleavedHandler(T impl)
        : self_(std::make_shared<Model<T>>(std::move(impl))) {}

    int onInterleaved(uint8_t ch, const uint8_t* p, size_t l) 
    {
        return self_->call(ch, p, l);
    }

private:
    struct Concept 
    {
        virtual ~Concept() = default;
        virtual int call(uint8_t, const uint8_t*, size_t) = 0;
    };

    template<typename T>
    struct Model : Concept 
    {
        T impl;
        Model(T v) : impl(std::move(v)) {}
        int call(uint8_t ch, const uint8_t* p, size_t l) override {
            return impl.onInterleaved(ch, p, l);
        }
    };

    std::shared_ptr<Concept> self_;
};



#endif //_INTERLEAVEDDISPATCHER_H_