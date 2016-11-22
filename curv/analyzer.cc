// Copyright Doug Moen 2016.
// Distributed under The MIT License.
// See accompanying file LICENSE.md or https://opensource.org/licenses/MIT

#include <curv/phrase.h>
#include <curv/analyzer.h>
#include <curv/shared.h>
#include <curv/exception.h>
#include <curv/thunk.h>
#include <curv/function.h>
#include <curv/context.h>

namespace curv
{

Shared<Operation>
analyze_op(const Phrase& ph, Environ& env)
{
    return ph.analyze(env)->to_operation(env);
}

Shared<Operation>
Metafunction::to_operation(Environ& env)
{
    throw Exception(At_Phrase(*source_, env), "not an operation");
}

Shared<Operation>
Operation::to_operation(Environ&)
{
    return share(*this);
}

Shared<Definition>
Phrase::analyze_def(Environ&) const
{
    return nullptr;
}

Shared<Meaning>
Environ::lookup(const Identifier& id)
{
    for (Environ* e = this; e != nullptr; e = e->parent_) {
        auto m = e->single_lookup(id);
        if (m != nullptr)
            return m;
    }
    throw Exception(At_Phrase(id, *this), stringify(id.atom_,": not defined"));
}

Shared<Meaning>
Builtin_Environ::single_lookup(const Identifier& id)
{
    auto p = names.find(id.atom_);
    if (p != names.end())
        return p->second->to_meaning(id);
    return nullptr;
}

void
Bindings::add_definition(Shared<Definition> def, curv::Environ& env)
{
    Atom name = def->name_->atom_;
    if (dictionary_->find(name) != dictionary_->end())
        throw Exception(At_Phrase(*def->name_, env),
            stringify(name, ": multiply defined"));
    (*dictionary_)[name] = cur_position_++;
    slot_phrases_.push_back(def->definiens_);

    auto lambda = dynamic_cast<Lambda_Phrase*>(def->definiens_.get());
    if (lambda != nullptr)
        lambda->recursive_ = true;
}

bool
Bindings::is_recursive_function(size_t slot)
{
    return isa_shared<const Lambda_Phrase>(slot_phrases_[slot]);
}

Shared<Meaning>
Bindings::Environ::single_lookup(const Identifier& id)
{
    auto b = bindings_.dictionary_->find(id.atom_);
    if (b != bindings_.dictionary_->end()) {
        if (bindings_.is_recursive_function(b->second))
            return make<Nonlocal_Function_Ref>(
                share(id), b->second);
        else
            return make<Module_Ref>(
                share(id), b->second);
    }
    return nullptr;
}

Shared<List>
Bindings::analyze_values(Environ& env)
{
    size_t n = slot_phrases_.size();
    auto slots = make_list(n);
    for (size_t i = 0; i < n; ++i) {
        auto expr = curv::analyze_op(*slot_phrases_[i], env);
        if (is_recursive_function(i)) {
            auto& l = dynamic_cast<Lambda_Expr&>(*expr);
            (*slots)[i] = {make<Lambda>(l.body_,l.nargs_,l.nslots_)};
        } else
            (*slots)[i] = {make<Thunk>(expr)};
    }
    return slots;
}

Shared<Meaning>
Identifier::analyze(Environ& env) const
{
    return env.lookup(*this);
}

Shared<Meaning>
Numeral::analyze(Environ& env) const
{
    std::string str(location().range());
    char* endptr;
    double n = strtod(str.c_str(), &endptr);
    assert(endptr == str.c_str() + str.size());
    return make<Constant>(share(*this), n);
}
Shared<Meaning>
String_Phrase::analyze(Environ& env) const
{
    auto str = location().range();
    assert(str.size() >= 2); // includes start and end " characters
    assert(*str.begin() == '"');
    assert(*(str.begin()+str.size()-1) == '"');
    ++str.first;
    --str.last;
    return make<Constant>(share(*this),
        Value{String::make(str.begin(),str.size())});
}

Shared<Meaning>
Unary_Phrase::analyze(Environ& env) const
{
    switch (op_.kind) {
    case Token::k_not:
        return make<Not_Expr>(
            share(*this),
            curv::analyze_op(*arg_, env));
    default:
        return make<Prefix_Expr>(
            share(*this),
            op_.kind,
            curv::analyze_op(*arg_, env));
    }
}

Shared<Meaning>
Lambda_Phrase::analyze(Environ& env) const
{
    // Syntax: id->expr or (a,b,...)->expr
    // TODO: pattern matching: [a,b]->expr, {a,b}->expr

    // phase 1: Create a dictionary of parameters.
    Atom_Map<int> params;
    int slot = 0;
    if (auto id = dynamic_cast<const Identifier*>(left_.get()))
    {
        params[id->atom_] = slot++;
    }
    else if (auto parens = dynamic_cast<const Paren_Phrase*>(left_.get()))
    {
        for (auto a : parens->args_) {
            if (auto id = dynamic_cast<const Identifier*>(a.expr_.get())) {
                params[id->atom_] = slot++;
            } else
                throw Exception(At_Phrase(*a.expr_, env), "not a parameter");
        }
    }
    else
        throw Exception(At_Phrase(*left_, env), "not a parameter");

    // Phase 2: make an Environ from the parameters and analyze the body.
    struct Arg_Environ : public Environ
    {
        Atom_Map<int>& names_;
        Module::Dictionary nonlocal_dictionary_;
        std::vector<Shared<const Operation>> nonlocal_exprs_;
        bool recursive_;

        Arg_Environ(Environ* parent, Atom_Map<int>& names, bool recursive)
        : Environ(parent), names_(names), recursive_(recursive)
        {
            frame_nslots = names.size();
            frame_maxslots = names.size();
        }
        virtual Shared<Meaning> single_lookup(const Identifier& id)
        {
            auto p = names_.find(id.atom_);
            if (p != names_.end())
                return make<Arg_Ref>(
                    share(id), p->second);
            if (recursive_)
                return nullptr;
            // In non-recursive mode, we return a definitive result.
            // We don't return nullptr (meaning try again in parent_).
            auto n = nonlocal_dictionary_.find(id.atom_);
            if (n != nonlocal_dictionary_.end()) {
                return make<Nonlocal_Ref>(
                    share(id), n->second);
            }
            auto m = parent_->lookup(id);
            if (isa_shared<Constant>(m))
                return m;
            if (auto expr = dynamic_shared_cast<Operation>(m)) {
                size_t slot = nonlocal_exprs_.size();
                nonlocal_dictionary_[id.atom_] = slot;
                nonlocal_exprs_.push_back(expr);
                return make<Nonlocal_Ref>(
                    share(id), slot);
            }
            return m;
        }
    };
    Arg_Environ env2(&env, params, recursive_);
    auto expr = curv::analyze_op(*right_, env2);
    auto src = share(*this);
    Shared<List_Expr> nonlocals =
        List_Expr::make(env2.nonlocal_exprs_.size(), src);
    // TODO: use some kind of Tail_Array move constructor
    for (size_t i = 0; i < env2.nonlocal_exprs_.size(); ++i)
        (*nonlocals)[i] = env2.nonlocal_exprs_[i];

    return make<Lambda_Expr>(
        src, expr, nonlocals, params.size(), env2.frame_maxslots);
}

Shared<Meaning>
Binary_Phrase::analyze(Environ& env) const
{
    switch (op_.kind) {
    case Token::k_or:
        return make<Or_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_and:
        return make<And_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_equal:
        return make<Equal_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_not_equal:
        return make<Not_Equal_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_less:
        return make<Less_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_greater:
        return make<Greater_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_less_or_equal:
        return make<Less_Or_Equal_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_greater_or_equal:
        return make<Greater_Or_Equal_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_power:
        return make<Power_Expr>(
            share(*this),
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    case Token::k_dot:
      {
        auto id = dynamic_shared_cast<Identifier>(right_);
        if (id != nullptr)
            return make<Dot_Expr>(
                share(*this),
                curv::analyze_op(*left_, env),
                id->atom_);
        auto list = dynamic_shared_cast<List_Phrase>(right_);
        if (list != nullptr) {
            Shared<Operation> index;
            if (list->args_.size() == 1
                && list->args_[0].comma_.kind == Token::k_missing)
            {
                index = curv::analyze_op(*list->args_[0].expr_, env);
            } else {
                throw Exception(At_Phrase(*this, env), "not an expression");
            }
            return make<At_Expr>(
                share(*this),
                curv::analyze_op(*left_, env),
                index);
        }
        throw Exception(At_Phrase(*right_, env),
            "invalid expression after '.'");
      }
    default:
        return make<Infix_Expr>(
            share(*this),
            op_.kind,
            curv::analyze_op(*left_, env),
            curv::analyze_op(*right_, env));
    }
}

Shared<Meaning>
Definition_Phrase::analyze(Environ& env) const
{
    throw Exception(At_Phrase(*this, env), "not an operation");
}

Shared<Definition>
Definition_Phrase::analyze_def(Environ& env) const
{
    if (auto id = dynamic_cast<const Identifier*>(left_.get())) {
        return make<Definition>(
            share(*id),
            right_);
    }

    if (auto call = dynamic_cast<const Call_Phrase*>(left_.get())) {
        if (auto id = dynamic_cast<const Identifier*>(call->function_.get())) {
            return make<Definition>(
                share(*id),
                make<Lambda_Phrase>(
                    call->args_,
                    equate_,
                    right_));
        } else {
            throw Exception(At_Phrase(*call->function_,  env),
                "not an identifier");
        }
    }

    throw Exception(At_Phrase(*left_,  env), "invalid definiendum");
}

Shared<Meaning>
Semicolon_Phrase::analyze(Environ& env) const
{
    if (args_.size() == 1)
    {
        return curv::analyze_op(*args_[0].expr_, env);
    } else {
        throw Exception(At_Phrase(*this,  env), "; phrase not implemented");
    /*
        Shared<Sequence_Expr> seq =
            Sequence_Expr::make(args_.size(), share(*this));
        for (size_t i = 0; i < args_.size(); ++i)
            (*seq)[i] = analyze_op(*args_[i].expr_, env);
        return seq;
    */
    }
}

Shared<Meaning>
Comma_Phrase::analyze(Environ& env) const
{
    if (args_.size() == 1
        && args_[0].comma_.kind == Token::k_missing)
    {
        return curv::analyze_op(*args_[0].expr_, env);
    } else {
        Shared<Sequence_Expr> seq =
            Sequence_Expr::make(args_.size(), share(*this));
        for (size_t i = 0; i < args_.size(); ++i)
            (*seq)[i] = analyze_op(*args_[i].expr_, env);
        return seq;
    }
}

Shared<Meaning>
Paren_Phrase::analyze(Environ& env) const
{
    if (args_.size() == 1
        && args_[0].comma_.kind == Token::k_missing)
    {
        return curv::analyze_op(*args_[0].expr_, env);
    } else {
        Shared<Sequence_Expr> seq =
            Sequence_Expr::make(args_.size(), share(*this));
        for (size_t i = 0; i < args_.size(); ++i)
            (*seq)[i] = analyze_op(*args_[i].expr_, env);
        return seq;
    }
}

Shared<Meaning>
List_Phrase::analyze(Environ& env) const
{
    Shared<List_Expr> list =
        List_Expr::make(args_.size(), share(*this));
    for (size_t i = 0; i < args_.size(); ++i)
        (*list)[i] = analyze_op(*args_[i].expr_, env);
    return list;
}

Shared<Meaning>
Call_Phrase::analyze(Environ& env) const
{
    return function_->analyze(env)->call(*this, env);
}

Shared<Meaning>
Operation::call(const Call_Phrase& src, Environ& env)
{
    auto argv = src.analyze_args(env);
    return make<Call_Expr>(
        share(src),
        share(*this),
        src.args_,
        std::move(argv));
}

std::vector<Shared<Operation>>
Call_Phrase::analyze_args(Environ& env) const
{
    std::vector<Shared<Operation>> argv;
    if (auto patom = dynamic_cast<Paren_Phrase*>(&*args_)) {
        // argument phrase is a variable-length parenthesized argument list
        argv.reserve(patom->args_.size());
        for (auto a : patom->args_)
            argv.push_back(curv::analyze_op(*a.expr_, env));
    } else {
        // argument phrase is a unitary expression
        argv.reserve(1);
        argv.push_back(curv::analyze_op(*args_, env));
    }
    return std::move(argv);
}

void
analyze_definition(
    const Definition& def,
    Atom_Map<Shared<const Operation>>& dict,
    Environ& env)
{
    Atom name = def.name_->atom_;
    if (dict.find(name) != dict.end())
        throw Exception(At_Phrase(*def.name_, env),
            stringify(name, ": multiply defined"));
    dict[name] = curv::analyze_op(*def.definiens_, env);
}

Shared<Meaning>
Module_Phrase::analyze(Environ& env) const
{
    return analyze_module(env);
}

/// An adapter class for iterating over the elements of a semicolon phrase.
struct Statements
{
    Statements(const Phrase& phrase)
    {
        auto semis = dynamic_cast<const Semicolon_Phrase*>(&phrase);
        if (semis) {
            first_ = &*semis->args_.front().expr_;
            rest_ = &semis->args_;
            size_ = semis->args_.size();
        } else {
            first_ = &phrase;
            rest_ = nullptr;
            size_ = 1;
        }
    }

    const Phrase* first_;
    const std::vector<Semicolon_Phrase::Arg>* rest_;
    size_t size_;

    struct iterator 
    {
        Statements& stmts_;
        size_t i_;
        iterator(Statements& stmts, size_t i) : stmts_(stmts), i_(i) {}
        const Phrase& operator*()
        {
            return i_==0 ? *stmts_.first_ : *(*stmts_.rest_)[i_].expr_;
        }
        const void operator++()
        {
            ++i_;
        }
        bool operator!=(iterator rhs)
        {
            return i_ != rhs.i_;
        }
    };
    iterator begin() { return iterator(*this, 0); }
    iterator end() { return iterator(*this, size_); }
};

Shared<Module_Expr>
Module_Phrase::analyze_module(Environ& env) const
{
    // phase 1: Create a dictionary of field phrases, a list of element phrases
    Bindings fields;
    std::vector<Shared<const Phrase>> elements;
    for (auto& st : Statements(*body_)) {
        auto def = st.analyze_def(env);
        if (def != nullptr)
            fields.add_definition(def, env);
        else
            elements.push_back(share(st));
    }

    // phase 2: Construct an environment from the field dictionary
    // and use it to perform semantic analysis.
    Bindings::Environ env2(&env, fields);
    auto self = share(*this);
    auto module = make<Module_Expr>(self);
    module->dictionary_ = fields.dictionary_;
    module->slots_ = fields.analyze_values(env2);
    Shared<List_Expr> xelements = {List_Expr::make(elements.size(), self)};
    for (size_t i = 0; i < elements.size(); ++i)
        (*xelements)[i] = curv::analyze_op(*elements[i], env2);
    module->elements_ = xelements;
    module->frame_nslots_ = env2.frame_maxslots;
    return module;
}

Shared<Meaning>
Record_Phrase::analyze(Environ& env) const
{
    Shared<Record_Expr> record =
        make<Record_Expr>(share(*this));
    for (auto i : args_) {
        auto def = i.expr_->analyze_def(env);
        if (def != nullptr) {
            analyze_definition(*def, record->fields_, env);
        } else {
            throw Exception(At_Phrase(*i.expr_, env), "not a definition");
        }
    }
    return record;
}

Shared<Meaning>
If_Phrase::analyze(Environ& env) const
{
    if (else_expr_ == nullptr) {
        return make<If_Expr>(
            share(*this),
            curv::analyze_op(*condition_, env),
            curv::analyze_op(*then_expr_, env));
    } else {
        return make<If_Else_Expr>(
            share(*this),
            curv::analyze_op(*condition_, env),
            curv::analyze_op(*then_expr_, env),
            curv::analyze_op(*else_expr_, env));
    }
}

Shared<Meaning>
Let_Phrase::analyze(Environ& env) const
{
    // `let` supports mutually recursive bindings, like let-rec in Scheme.
    //
    // To evaluate: lazy evaluation of thunks, error on illegal recursion.
    // These thunks do not get a fresh evaluation Frame, they use the Frame
    // of the `let` expression. That's consistent with an optimizing compiler
    // where let bindings are SSA values.
    //
    // To analyze: we assign a slot number to each of the let bindings,
    // *then* we construct an Environ and analyze each definiens.
    // This means no opportunity for optimization (eg, don't store a let binding
    // in a slot or create a Thunk if it is a Constant). To optimize, we need
    // another compiler pass or two, so that we do register allocation *after*
    // analysis and constant folding is complete.

    // phase 1: Create a dictionary of bindings.
    struct Binding
    {
        int slot_;
        Shared<Phrase> phrase_;
        Binding(int slot, Shared<Phrase> phrase)
        : slot_(slot), phrase_(phrase)
        {}
        Binding(){}
    };
    Atom_Map<Binding> bindings;
    int slot = env.frame_nslots;
    for (auto b : args_->args_) {
        auto def = b.expr_->analyze_def(env);
        if (def == nullptr)
            throw Exception(At_Phrase(*b.expr_, env), "not a definition");
        Atom name = def->name_->atom_;
        if (bindings.find(name) != bindings.end())
            throw Exception(At_Phrase(*def->name_, env),
                stringify(name, ": multiply defined"));
        bindings[name] = Binding{slot++, def->definiens_};
    }

    // phase 2: Construct an environment from the bindings dictionary
    // and use it to perform semantic analysis.
    struct Let_Environ : public Environ
    {
    protected:
        const Atom_Map<Binding>& names;
    public:
        Let_Environ(
            Environ* p,
            const Atom_Map<Binding>& n)
        : Environ(p), names(n)
        {
            if (p != nullptr) {
                frame_nslots = p->frame_nslots;
                frame_maxslots = p->frame_maxslots;
            }
            frame_nslots += n.size();
            frame_maxslots = std::max(frame_maxslots, frame_nslots);
        }
        virtual Shared<Meaning> single_lookup(const Identifier& id)
        {
            auto p = names.find(id.atom_);
            if (p != names.end())
                return make<Let_Ref>(
                    share(id), p->second.slot_);
            return nullptr;
        }
    };
    Let_Environ env2(&env, bindings);

    int first_slot = env.frame_nslots;
    std::vector<Value> values(bindings.size());
    for (auto b : bindings) {
        auto expr = curv::analyze_op(*b.second.phrase_, env2);
        values[b.second.slot_-first_slot] = {make<Thunk>(expr)};
    }
    auto body = curv::analyze_op(*body_, env2);
    env.frame_maxslots = env2.frame_maxslots;
    assert(env.frame_maxslots >= bindings.size());

    return make<Let_Expr>(share(*this),
        first_slot, std::move(values), body);
}

Shared<Meaning>
For_Phrase::analyze(Environ& env) const
{
    if (args_->args_.size() != 1)
        throw Exception(At_Phrase(*args_, env), "for: malformed argument");

    auto defexpr = args_->args_[0].expr_;
    const Definition_Phrase* def = dynamic_cast<Definition_Phrase*>(&*defexpr);
    if (def == nullptr)
        throw Exception(At_Phrase(*defexpr, env),
            "for: not a definition");
    auto id = dynamic_cast<const Identifier*>(def->left_.get());
    if (id == nullptr)
        throw Exception(At_Phrase(*def->left_, env), "for: not an identifier");
    Atom name = id->atom_;

    auto list = curv::analyze_op(*def->right_, env);

    int slot = env.frame_nslots;
    struct For_Environ : public Environ
    {
        Atom name_;
        int slot_;

        For_Environ(
            Environ& p,
            Atom name,
            int slot)
        : Environ(&p), name_(name), slot_(slot)
        {
            frame_nslots = p.frame_nslots + 1;
            frame_maxslots = std::max(p.frame_maxslots, frame_nslots);
        }
        virtual Shared<Meaning> single_lookup(const Identifier& id)
        {
            if (id.atom_ == name_)
                return make<Let_Ref>(
                    share(id), slot_);
            return nullptr;
        }
    };
    For_Environ body_env(env, name, slot);
    auto body = curv::analyze_op(*body_, body_env);
    env.frame_maxslots = body_env.frame_maxslots;

    return make<For_Expr>(share(*this),
        slot, list, body);
}

Shared<Meaning>
Range_Phrase::analyze(Environ& env) const
{
    return make<Range_Gen>(
        share(*this),
        curv::analyze_op(*first_, env),
        curv::analyze_op(*last_, env),
        step_ ? curv::analyze_op(*step_, env) : nullptr);
}

} // namespace curv
