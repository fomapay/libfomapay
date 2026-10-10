#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace fomapay::json {

const Value* Value::find(const std::string& key) const {
    for (const auto& member : object) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::string Value::str(const std::string& key) const {
    const Value* value = find(key);
    return value && value->type == Type::String ? value->string : "";
}

double Value::num(const std::string& key) const {
    const Value* value = find(key);
    return value && value->type == Type::Number ? value->number : 0;
}

bool Value::flag(const std::string& key) const {
    const Value* value = find(key);
    return value && value->type == Type::Bool && value->boolean;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value document() {
        spaces();
        Value value = read();
        spaces();
        if (pos_ != text_.size()) {
            fail("contenu inattendu après la valeur");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::invalid_argument("JSON invalide (position " + std::to_string(pos_) + ") : " + message);
    }

    char peek() const {
        if (pos_ >= text_.size()) {
            fail("fin inattendue");
        }
        return text_[pos_];
    }

    void spaces() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool literal(const char* word) {
        const std::string w(word);
        if (text_.compare(pos_, w.size(), w) == 0) {
            pos_ += w.size();
            return true;
        }
        return false;
    }

    Value read() {
        if (++depth_ > 64) {
            fail("imbrication trop profonde");
        }
        Value value;
        const char c = peek();
        if (c == '{') {
            value.type = Value::Type::Object;
            ++pos_;
            spaces();
            if (peek() == '}') {
                ++pos_;
            } else {
                for (;;) {
                    spaces();
                    if (peek() != '"') {
                        fail("clé attendue");
                    }
                    std::string key = text();
                    spaces();
                    if (peek() != ':') {
                        fail("« : » attendu");
                    }
                    ++pos_;
                    spaces();
                    value.object.emplace_back(std::move(key), read());
                    spaces();
                    if (peek() == ',') {
                        ++pos_;
                        continue;
                    }
                    if (peek() != '}') {
                        fail("« } » attendu");
                    }
                    ++pos_;
                    break;
                }
            }
        } else if (c == '[') {
            value.type = Value::Type::Array;
            ++pos_;
            spaces();
            if (peek() == ']') {
                ++pos_;
            } else {
                for (;;) {
                    spaces();
                    value.array.push_back(read());
                    spaces();
                    if (peek() == ',') {
                        ++pos_;
                        continue;
                    }
                    if (peek() != ']') {
                        fail("« ] » attendu");
                    }
                    ++pos_;
                    break;
                }
            }
        } else if (c == '"') {
            value.type = Value::Type::String;
            value.string = text();
        } else if (literal("true")) {
            value.type = Value::Type::Bool;
            value.boolean = true;
        } else if (literal("false")) {
            value.type = Value::Type::Bool;
        } else if (literal("null")) {
            value.type = Value::Type::Null;
        } else {
            value.type = Value::Type::Number;
            value.number = number();
        }
        --depth_;
        return value;
    }

    bool digit() const { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; }

    // Lecture manuelle : strtod dépend de la locale (virgule décimale dans une application en français).
    double number() {
        const std::size_t start = pos_;
        const bool negative = pos_ < text_.size() && text_[pos_] == '-';
        if (negative) {
            ++pos_;
        }
        if (!digit()) {
            pos_ = start;
            fail("valeur inattendue");
        }
        double result = 0;
        while (digit()) {
            result = result * 10 + (text_[pos_++] - '0');
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            double scale = 0.1;
            while (digit()) {
                result += (text_[pos_++] - '0') * scale;
                scale /= 10;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            const bool down = pos_ < text_.size() && text_[pos_] == '-';
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
                ++pos_;
            }
            int exponent = 0;
            while (digit() && exponent < 400) {
                exponent = exponent * 10 + (text_[pos_++] - '0');
            }
            result *= std::pow(10.0, down ? -exponent : exponent);
        }
        return negative ? -result : result;
    }

    void utf8(unsigned code, std::string& out) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    unsigned hex4() {
        if (pos_ + 4 > text_.size()) {
            fail("échappement unicode incomplet");
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
                fail("échappement unicode invalide");
            }
        }
        return code;
    }

    std::string text() {
        std::string out;
        ++pos_;
        for (;;) {
            if (pos_ >= text_.size()) {
                fail("chaîne non terminée");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("échappement incomplet");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': case '\\': case '/': out += escaped; break;
                case 'u': {
                    unsigned code = hex4();
                    // Paire de substitution (caractères hors du plan de base, ex. emoji).
                    if (code >= 0xD800 && code <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
                        pos_ += 2;
                        const unsigned low = hex4();
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8(code, out);
                    break;
                }
                default: fail("échappement inconnu");
            }
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

}  // namespace

Value parse(const std::string& text) { return Parser(text).document(); }

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

void Writer::key(const std::string& name) {
    if (!out_.empty()) {
        out_ += ',';
    }
    out_ += quote(name) + ":";
}

Writer& Writer::string(const std::string& name, const std::string& value) {
    key(name);
    out_ += quote(value);
    return *this;
}

Writer& Writer::optional(const std::string& name, const std::string& value) {
    return value.empty() ? *this : string(name, value);
}

Writer& Writer::number(const std::string& name, double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.12g", value);
    key(name);
    for (const char* c = buffer; *c; ++c) {
        out_ += *c == ',' ? '.' : *c;  // snprintf suit la locale : la virgule décimale redevient un point
    }
    return *this;
}

Writer& Writer::raw(const std::string& name, const std::string& json) {
    key(name);
    out_ += json;
    return *this;
}

}  // namespace fomapay::json
