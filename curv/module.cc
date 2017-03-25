// Copyright Doug Moen 2016-2017.
// Distributed under The MIT License.
// See accompanying file LICENSE.md or https://opensource.org/licenses/MIT

#include <curv/module.h>
#include <curv/function.h>

namespace curv {

const char Module::name[] = "module";

void
Module::print(std::ostream& out) const
{
    out << "{";
    bool first = true;
    for (auto i : *this) {
        if (!first) out << ",";
        first = false;
        out << i.first << "=";
        i.second.print(out);
    }
    out << "}";
}

Value
Module::get(slot_t i) const
{
    Value val = (*slots_)[i];
    if (val.is_ref()) {
        auto& ref = val.get_ref_unsafe();
        if (ref.type_ == Ref_Value::ty_lambda)
            return {make<Closure>((Lambda&)ref, *slots_)};
    }
    return val;
}

auto Module::getfield(Atom name) const
-> Value
{
    auto b = dictionary_->find(name);
    if (b != dictionary_->end())
        return get(b->second);
    return missing;
}

} // namespace curv
