// OhMyDisplay - minimal self-contained JSON value model (no external runtime).
// Supports null/bool/number/string/array/object. Objects preserve insertion order
// for deterministic output. UTF-8 strings throughout.
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <cstdint>

namespace omd::json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    class Value;
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    class Value
    {
    public:
        Value() : type_(Type::Null) {}
        Value(std::nullptr_t) : type_(Type::Null) {}
        Value(bool b) : type_(Type::Bool), bool_(b) {}
        Value(double n) : type_(Type::Number), num_(n) {}
        Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
        Value(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
        Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
        Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

        static Value MakeArray()
        {
            Value v;
            v.type_ = Type::Array;
            return v;
        }
        static Value MakeObject()
        {
            Value v;
            v.type_ = Type::Object;
            return v;
        }

        Type GetType() const
        {
            return type_;
        }
        bool IsNull() const
        {
            return type_ == Type::Null;
        }
        bool IsObject() const
        {
            return type_ == Type::Object;
        }
        bool IsArray() const
        {
            return type_ == Type::Array;
        }
        bool IsString() const
        {
            return type_ == Type::String;
        }
        bool IsNumber() const
        {
            return type_ == Type::Number;
        }
        bool IsBool() const
        {
            return type_ == Type::Bool;
        }

        // Scalar accessors with defaults (safe on type mismatch).
        bool AsBool(bool def = false) const
        {
            return type_ == Type::Bool ? bool_ : def;
        }
        double AsNumber(double def = 0.0) const
        {
            return type_ == Type::Number ? num_ : def;
        }
        int AsInt(int def = 0) const
        {
            return type_ == Type::Number ? static_cast<int>(num_) : def;
        }
        const std::string& AsString() const
        {
            return type_ == Type::String ? str_ : empty_string_;
        }
        std::string AsStringOr(const std::string& def) const
        {
            return type_ == Type::String ? str_ : def;
        }

        // Array access.
        Array& AsArray()
        {
            EnsureArray();
            return arr_;
        }
        const Array& AsArray() const
        {
            return arr_;
        }
        void Push(Value v)
        {
            EnsureArray();
            arr_.push_back(std::move(v));
        }

        // Object access. Set overwrites an existing key.
        void Set(const std::string& key, Value v)
        {
            EnsureObject();
            for (auto& m : obj_)
            {
                if (m.first == key)
                {
                    m.second = std::move(v);
                    return;
                }
            }
            obj_.emplace_back(key, std::move(v));
        }
        const Value* Find(const std::string& key) const
        {
            if (type_ != Type::Object)
            {
                return nullptr;
            }
            for (const auto& m : obj_)
            {
                if (m.first == key)
                {
                    return &m.second;
                }
            }
            return nullptr;
        }
        // Returns a null sentinel reference when key is absent.
        const Value& operator[](const std::string& key) const
        {
            const Value* p = Find(key);
            return p ? *p : null_sentinel_;
        }
        const Object& Members() const
        {
            return obj_;
        }

        // Serialize. pretty=true uses two-space indentation.
        std::string Dump(bool pretty = true) const;

        // Parse from UTF-8 text. Returns false on error; errPos is byte offset.
        static bool Parse(std::string_view text, Value& out, std::string* error = nullptr);

    private:
        void EnsureArray()
        {
            if (type_ != Type::Array)
            {
                type_ = Type::Array;
                arr_.clear();
            }
        }
        void EnsureObject()
        {
            if (type_ != Type::Object)
            {
                type_ = Type::Object;
                obj_.clear();
            }
        }

        void DumpTo(std::string& out, bool pretty, int depth) const;

        Type type_;
        bool bool_ = false;
        double num_ = 0.0;
        std::string str_;
        Array arr_;
        Object obj_;

        static const std::string empty_string_;
        static const Value null_sentinel_;
    };
}
