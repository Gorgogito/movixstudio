#pragma once

#include <QHash>
#include <QString>

#include <functional>
#include <optional>
#include <string>

// Fonemas del inglés americano para Kokoro: adaptación a C++ del léxico de misaki
// (https://github.com/hexgrad/misaki, Apache-2.0), el fonetizador con el que se entrenó el modelo.
// Sin el etiquetador gramatical de misaki: las palabras que se pronuncian distinto según su función
// («read», «live», «record»...) usan la pronunciación habitual. Lo que no está en el léxico
// (números, nombres poco comunes...) se fonetiza aparte, normalmente con eSpeak (ver
// englishFromEspeak).
namespace movix::ai {

class EnglishLexicon {
public:
    // us_gold.json y us_silver.json de misaki.
    bool load(const std::string& folder, std::string* error = nullptr);
    bool loaded() const { return !m_golds.isEmpty(); }
    const std::string& folder() const { return m_folder; }

    // Contexto de una palabra: si la siguiente empieza por vocal (vacío si no hay más palabras en la
    // cláusula) y si la siguiente es «to».
    struct Context {
        std::optional<bool> futureVowel;
        bool futureTo = false;
    };
    // Fonemas de una palabra (sin espacios ni puntuación), o nada si no está en el léxico.
    std::optional<QString> word(const QString& word, const Context& ctx = {}) const;
    // Fonemas de un texto con su puntuación, en el formato de Kokoro. `fallback` fonetiza lo que no
    // está en el léxico (una palabra o un grupo sin espacios) en ese mismo formato.
    QString text(const QString& text, const std::function<QString(const QString&)>& fallback) const;

private:
    struct Entry {
        QString plain;
        QHash<QString, std::optional<QString>> tags; // si la pronunciación depende de la función
        bool isDict = false;
    };
    const Entry* find(const QString& word) const; // primero gold, luego silver
    std::optional<QString> lookup(QString word, std::optional<double> stress, const Context& ctx) const;
    std::optional<QString> spelled(const QString& word) const; // siglas, letra a letra
    std::optional<QString> specialCase(const QString& word, std::optional<double> stress, const Context& ctx) const;
    bool isKnown(const QString& word) const;
    std::optional<QString> stemS(const QString& word, std::optional<double> stress, const Context& ctx) const;
    std::optional<QString> stemEd(const QString& word, std::optional<double> stress, const Context& ctx) const;
    std::optional<QString> stemIng(const QString& word, std::optional<double> stress, const Context& ctx) const;
    std::optional<QString> getWord(QString word, std::optional<double> stress, const Context& ctx) const;

    QHash<QString, Entry> m_golds, m_silvers;
    std::string m_folder;
};

// Fonemas de eSpeak NG (inglés americano, IPA con '^' uniendo los símbolos compuestos) en el formato
// de misaki, como hace su respaldo con eSpeak.
QString englishFromEspeak(QString ps);

} // namespace movix::ai
