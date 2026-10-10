// JSON minimal interne : libfomapay n'impose aucune bibliothèque JSON.
#ifndef FOMAPAY_JSON_HPP
#define FOMAPAY_JSON_HPP

#include <string>
#include <utility>
#include <vector>

namespace fomapay::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    /// Membre d'un objet, ou nullptr.
    const Value* find(const std::string& key) const;
    /// Chaîne du membre, ou "" s'il est absent ou d'un autre type.
    std::string str(const std::string& key) const;
    double num(const std::string& key) const;
    bool flag(const std::string& key) const;
};

/// @throws std::invalid_argument si le texte n'est pas du JSON valide
Value parse(const std::string& text);

/// Écrit un objet JSON champ par champ.
class Writer {
public:
    Writer& string(const std::string& key, const std::string& value);
    /// N'écrit rien si la valeur est vide.
    Writer& optional(const std::string& key, const std::string& value);
    Writer& number(const std::string& key, double value);
    /// Valeur déjà encodée en JSON (objet ou tableau imbriqué).
    Writer& raw(const std::string& key, const std::string& json);
    std::string str() const { return "{" + out_ + "}"; }
    bool empty() const { return out_.empty(); }

private:
    void key(const std::string& name);
    std::string out_;
};

std::string quote(const std::string& text);

}  // namespace fomapay::json

#endif  // FOMAPAY_JSON_HPP
