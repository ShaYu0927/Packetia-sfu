#include "sip.h"

#include <algorithm>
#include <cctype>

namespace
{
std::string ToLower(std::string s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
} // namespace

void SipHeaders::build_index()
{
    index.clear();
    for (size_t i = 0; i < list.size(); ++i)
    {
        index[ToLower(list[i].name)].push_back(i);
    }
}

std::string SipHeaders::get_one(std::string_view name) const
{
    std::string key(name);
    key = ToLower(std::move(key));
    auto it = index.find(key);
    if (it == index.end() || it->second.empty()) return "";
    return list[it->second.front()].value;
}

std::vector<std::string> SipHeaders::get_all(std::string_view name) const
{
    std::vector<std::string> values;
    std::string key(name);
    key = ToLower(std::move(key));
    auto it = index.find(key);
    if (it == index.end()) return values;

    values.reserve(it->second.size());
    for (size_t idx : it->second)
    {
        if (idx < list.size()) values.push_back(list[idx].value);
    }
    return values;
}

void SipHeaders::add(std::string name, std::string value)
{
    size_t idx = list.size();
    std::string key = ToLower(name);
    list.push_back(SipHeader{std::move(name), std::move(value)});
    index[key].push_back(idx);
}