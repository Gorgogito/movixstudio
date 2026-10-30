// Adaptación del léxico de misaki (misaki/en.py, Apache-2.0, © hexgrad): mismas reglas y mismos
// nombres para poder compararlos.
#include "ai/EnglishG2P.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace movix::ai {

namespace {

constexpr QChar kPrimary = u'ˈ';
constexpr QChar kSecondary = u'ˌ';
const QString kStresses = QStringLiteral(u"ˌˈ");
const QString kVowels = QStringLiteral(u"AIOQWYaiuæɑɒɔəɛɜɪʊʌᵻ");
const QString kConsonants = QStringLiteral(u"bdfhjklmnpstvwzðŋɡɹɾʃʒʤʧθ");
const QString kDiphthongs = QStringLiteral(u"AIOQWYʤʧ");
const QString kUsTaus = QStringLiteral(u"AIOWYiuæɑəɛɪɹʊʌ");
const QString kPuncts = QStringLiteral(u";:,.!?—…\"“”");
const QString kNonQuotePuncts = QStringLiteral(u";:,.!?—…");
const QString kSubtokenJunks = QStringLiteral(u"',-._‘’/");

bool isAlpha(const QString& s)
{
    return !s.isEmpty() && std::all_of(s.begin(), s.end(), [](QChar c) { return c.isLetter(); });
}

bool isLower(const QString& s) { return s == s.toLower(); }
bool isUpper(const QString& s) { return s == s.toUpper(); }

QString capitalize(const QString& s)
{
    return s.isEmpty() ? s : s.left(1).toUpper() + s.mid(1).toLower();
}

bool hasAny(const QString& s, const QString& chars)
{
    return std::any_of(s.begin(), s.end(), [&](QChar c) { return chars.contains(c); });
}

bool allIn(const QString& s, const QString& chars)
{
    return std::all_of(s.begin(), s.end(), [&](QChar c) { return chars.contains(c); });
}

// Letras ASCII, apóstrofo y guion (LEXICON_ORDS).
bool lexiconChars(const QString& s)
{
    return std::all_of(s.begin(), s.end(), [](QChar c) {
        const char16_t u = c.unicode();
        return u == '\'' || u == '-' || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
    });
}

// Lleva cada marca de acento justo antes de la vocal siguiente.
QString restress(const QString& ps)
{
    std::vector<std::pair<double, QChar>> ips;
    for (qsizetype i = 0; i < ps.size(); ++i)
        ips.emplace_back(double(i), ps[i]);
    for (qsizetype i = 0; i < ps.size(); ++i) {
        if (!kStresses.contains(ps[i]))
            continue;
        for (qsizetype j = i; j < ps.size(); ++j)
            if (kVowels.contains(ps[j])) {
                ips[size_t(i)].first = double(j) - 0.5;
                break;
            }
    }
    std::stable_sort(ips.begin(), ips.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QString out;
    for (const auto& [pos, c] : ips)
        out += c;
    return out;
}

QString applyStress(QString ps, std::optional<double> stress)
{
    if (!stress)
        return ps;
    const double s = *stress;
    const bool primary = ps.contains(kPrimary), any = hasAny(ps, kStresses);
    if (s < -1)
        return ps.remove(kPrimary).remove(kSecondary);
    if (s == -1 || ((s == 0 || s == -0.5) && primary))
        return ps.remove(kSecondary).replace(kPrimary, kSecondary);
    if ((s == 0 || s == 0.5 || s == 1) && !any)
        return hasAny(ps, kVowels) ? restress(kSecondary + ps) : ps;
    if (s >= 1 && !primary && ps.contains(kSecondary))
        return ps.replace(kSecondary, kPrimary);
    if (s > 1 && !any)
        return hasAny(ps, kVowels) ? restress(kPrimary + ps) : ps;
    return ps;
}

int stressWeight(const QString& ps)
{
    int w = 0;
    for (QChar c : ps)
        w += kDiphthongs.contains(c) ? 2 : 1;
    return w;
}

QString suffixS(const QString& stem)
{
    if (stem.isEmpty())
        return {};
    const QChar last = stem.back();
    if (QStringLiteral(u"ptkfθ").contains(last))
        return stem + u's';
    if (QStringLiteral(u"szʃʒʧʤ").contains(last))
        return stem + QStringLiteral(u"ᵻz");
    return stem + u'z';
}

QString suffixEd(const QString& stem)
{
    if (stem.isEmpty())
        return {};
    const QChar last = stem.back();
    if (QStringLiteral(u"pkfθʃsʧ").contains(last))
        return stem + u't';
    if (last == u'd')
        return stem + QStringLiteral(u"ᵻd");
    if (last != u't')
        return stem + u'd';
    if (stem.size() < 2)
        return stem + QStringLiteral(u"ɪd");
    if (kUsTaus.contains(stem[stem.size() - 2]))
        return stem.chopped(1) + QStringLiteral(u"ɾᵻd");
    return stem + QStringLiteral(u"ᵻd");
}

QString suffixIng(const QString& stem)
{
    if (stem.isEmpty())
        return {};
    if (stem.size() > 1 && stem.back() == u't' && kUsTaus.contains(stem[stem.size() - 2]))
        return stem.chopped(1) + QStringLiteral(u"ɾɪŋ");
    return stem + QStringLiteral(u"ɪŋ");
}

std::optional<QString> withSuffix(const std::optional<QString>& stem, QString (*suffix)(const QString&))
{
    if (!stem || stem->isEmpty())
        return std::nullopt;
    return suffix(*stem);
}

// Partes de una palabra sin espacios (subtokenize de misaki) y, como hace spaCy antes, las
// contracciones aparte («don't» → «do» «n't»): se busca primero el grupo entero.
QStringList subtokenize(const QString& word)
{
    static const QRegularExpression re(QStringLiteral(u"^['‘’]+|\\p{Lu}(?=\\p{Lu}\\p{Ll})|(?:^-)?(?:\\d?[,.]?\\d)+|[-_]+|['‘’]{2,}|"
                                                      u"\\p{L}*?(?:['‘’]\\p{L})*?\\p{Ll}(?=\\p{Lu})|\\p{L}+(?:['‘’]\\p{L})*|"
                                                      u"[^-_\\p{L}'‘’\\d]|['‘’]+$"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression contraction(QStringLiteral(u"^(\\p{L}+)(n['‘’]t|['‘’](?:s|m|re|ve|ll|d))$"),
                                                QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    for (auto it = re.globalMatch(word); it.hasNext();) {
        const QString part = it.next().captured();
        if (const auto m = contraction.match(part); m.hasMatch()) {
            out << m.captured(1) << m.captured(2);
            continue;
        }
        out << part;
    }
    return out;
}

std::optional<bool> futureVowelOf(const QString& ps, std::optional<bool> current)
{
    for (QChar c : ps) {
        if (kNonQuotePuncts.contains(c))
            return std::nullopt;
        if (kVowels.contains(c))
            return true;
        if (kConsonants.contains(c))
            return false;
    }
    return current;
}

EnglishLexicon::Context contextAfter(const EnglishLexicon::Context& ctx, const std::optional<QString>& ps, const QString& text)
{
    EnglishLexicon::Context next;
    next.futureVowel = ps && !ps->isEmpty() ? futureVowelOf(*ps, ctx.futureVowel) : ctx.futureVowel;
    next.futureTo = text == QLatin1String("to") || text == QLatin1String("To") || text == QLatin1String("TO");
    return next;
}

// Grupo sin espacios ya resuelto parte a parte: acentos (los compuestos guardan uno principal) y
// unión de los fonemas, con espacio si mezcla letras, cifras y otros signos.
QString resolveGroup(const QStringList& parts, std::vector<std::optional<QString>>& ps)
{
    const QString text = parts.join(QString());
    QSet<int> classes;
    for (QChar c : text)
        if (!kSubtokenJunks.contains(c))
            classes.insert(c.isLetter() ? 0 : c.isDigit() ? 1 : 2);
    const bool prespace = text.contains(u' ') || text.contains(u'/') || classes.size() > 1;
    for (size_t i = 0; i < ps.size(); ++i)
        if (!ps[i]) {
            if (i + 1 == ps.size() && parts[qsizetype(i)].size() == 1 && kNonQuotePuncts.contains(parts[qsizetype(i)]))
                ps[i] = parts[qsizetype(i)];
            else if (allIn(parts[qsizetype(i)], kSubtokenJunks))
                ps[i] = QString();
        }
    if (!prespace) {
        struct Index {
            bool primary;
            int weight;
            size_t i;
            bool operator<(const Index& o) const { return std::tie(primary, weight, i) < std::tie(o.primary, o.weight, o.i); }
        };
        std::vector<Index> indices;
        for (size_t i = 0; i < ps.size(); ++i)
            if (ps[i] && !ps[i]->isEmpty())
                indices.push_back({ps[i]->contains(kPrimary), stressWeight(*ps[i]), i});
        const size_t primaries = size_t(std::count_if(indices.begin(), indices.end(), [](const Index& x) { return x.primary; }));
        if (indices.size() == 2 && parts[qsizetype(indices[0].i)].size() == 1) {
            ps[indices[1].i] = applyStress(*ps[indices[1].i], -0.5);
        } else if (indices.size() >= 2 && primaries > (indices.size() + 1) / 2) {
            std::sort(indices.begin(), indices.end());
            for (size_t k = 0; k < indices.size() / 2; ++k)
                ps[indices[k].i] = applyStress(*ps[indices[k].i], -0.5);
        }
    }
    QString out;
    for (size_t i = 0; i < ps.size(); ++i) {
        const bool spaced = prespace && i > 0 && ps[i] && !ps[i]->isEmpty();
        if (spaced && !out.isEmpty() && !out.back().isSpace())
            out += u' ';
        out += ps[i].value_or(QString());
    }
    return out;
}

} // namespace

bool EnglishLexicon::load(const std::string& folder, std::string* error)
{
    if (loaded() && m_folder == folder)
        return true;
    auto read = [&](const char* name, QHash<QString, Entry>& out) {
        QFile f(QString::fromStdString(folder) + QLatin1Char('/') + QLatin1String(name));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
        QHash<QString, Entry> d;
        d.reserve(json.size());
        for (auto it = json.begin(); it != json.end(); ++it) {
            Entry e;
            if (it.value().isObject()) {
                e.isDict = true;
                const QJsonObject tags = it.value().toObject();
                for (auto t = tags.begin(); t != tags.end(); ++t)
                    e.tags.insert(t.key(), t.value().isString() ? std::optional<QString>(t.value().toString()) : std::nullopt);
            } else {
                e.plain = it.value().toString();
            }
            d.insert(it.key(), std::move(e));
        }
        // grow_dictionary: también en mayúscula inicial o en minúsculas (sin pisar lo que hay).
        out = d;
        for (auto it = d.cbegin(); it != d.cend(); ++it) {
            const QString& k = it.key();
            if (k.size() < 2)
                continue;
            QString variant;
            if (isLower(k)) {
                if (k != capitalize(k))
                    variant = capitalize(k);
            } else if (k == capitalize(k.toLower())) {
                variant = k.toLower();
            }
            if (!variant.isEmpty() && !d.contains(variant))
                out.insert(variant, it.value());
        }
        return !out.isEmpty();
    };
    QHash<QString, Entry> golds, silvers;
    if (!read("us_gold.json", golds) || !read("us_silver.json", silvers)) {
        if (error)
            *error = "No se pudo leer el léxico inglés de " + folder;
        return false;
    }
    m_golds = std::move(golds);
    m_silvers = std::move(silvers);
    m_folder = folder;
    return true;
}

const EnglishLexicon::Entry* EnglishLexicon::find(const QString& word) const
{
    if (auto it = m_golds.constFind(word); it != m_golds.cend())
        return &*it;
    if (auto it = m_silvers.constFind(word); it != m_silvers.cend())
        return &*it;
    return nullptr;
}

std::optional<QString> EnglishLexicon::spelled(const QString& word) const
{
    QString ps;
    for (QChar c : word) {
        if (!c.isLetter())
            continue;
        auto it = m_golds.constFind(QString(c).toUpper());
        if (it == m_golds.cend())
            return std::nullopt;
        ps += it->isDict ? it->tags.value(QStringLiteral("DEFAULT")).value_or(QString()) : it->plain;
    }
    ps = applyStress(ps, 0);
    // El último acento secundario pasa a principal.
    if (const qsizetype last = ps.lastIndexOf(kSecondary); last >= 0)
        ps[last] = kPrimary;
    return ps;
}

std::optional<QString> EnglishLexicon::lookup(QString word, std::optional<double> stress, const Context& ctx) const
{
    if (isUpper(word) && !m_golds.contains(word))
        word = word.toLower();
    const Entry* e = find(word);
    std::optional<QString> ps;
    if (e && !e->isDict) {
        ps = e->plain;
    } else if (e) {
        const QString tag = !ctx.futureVowel && e->tags.contains(QStringLiteral("None")) ? QStringLiteral("None") : QStringLiteral("DEFAULT");
        ps = e->tags.value(tag, e->tags.value(QStringLiteral("DEFAULT")));
    }
    if (!ps)
        return spelled(word);
    return applyStress(*ps, stress);
}

std::optional<QString> EnglishLexicon::specialCase(const QString& word, std::optional<double> stress, const Context& ctx) const
{
    static const QHash<QString, QString> symbols = {{QStringLiteral("%"), QStringLiteral("percent")},
                                                     {QStringLiteral("&"), QStringLiteral("and")},
                                                     {QStringLiteral("+"), QStringLiteral("plus")},
                                                     {QStringLiteral("@"), QStringLiteral("at")}};
    auto is = [&](std::initializer_list<const char*> forms) {
        return std::any_of(forms.begin(), forms.end(), [&](const char* f) { return word == QLatin1String(f); });
    };
    auto gold = [&](const char* w, const char* tag = "DEFAULT") -> QString {
        const Entry& e = m_golds[QLatin1String(w)];
        return e.isDict ? e.tags.value(QLatin1String(tag)).value_or(QString()) : e.plain;
    };
    if (symbols.contains(word))
        return lookup(symbols[word], std::nullopt, ctx);
    // Abreviaturas con puntos («U.S»): letra a letra.
    QString trimmed = word;
    while (trimmed.startsWith(u'.'))
        trimmed.remove(0, 1);
    while (trimmed.endsWith(u'.'))
        trimmed.chop(1);
    if (trimmed.contains(u'.') && isAlpha(QString(word).remove(u'.'))) {
        qsizetype longest = 0;
        for (const QString& part : word.split(u'.'))
            longest = std::max(longest, part.size());
        if (longest < 3)
            return spelled(word);
    }
    // Sin etiquetas gramaticales: «a» como artículo, «I» como pronombre, «in» como preposición.
    if (is({"a", "A"}))
        return QStringLiteral(u"ɐ");
    if (is({"am", "Am", "AM"}))
        return !ctx.futureVowel || word != QLatin1String("am") || (stress && *stress > 0) ? gold("am") : QStringLiteral(u"ɐm");
    if (is({"an", "An", "AN"}))
        return QStringLiteral(u"ɐn");
    if (word == QLatin1String("I"))
        return QStringLiteral(u"ˌI");
    if (is({"to", "To", "TO"}))
        return !ctx.futureVowel ? gold("to") : *ctx.futureVowel ? QStringLiteral(u"tʊ") : QStringLiteral(u"tə");
    if (is({"in", "In", "IN"}))
        return (!ctx.futureVowel ? QString(kPrimary) : QString()) + QStringLiteral(u"ɪn");
    if (is({"the", "The", "THE"}))
        return ctx.futureVowel == true ? QStringLiteral(u"ði") : QStringLiteral(u"ðə");
    if (word.compare(QLatin1String("vs"), Qt::CaseInsensitive) == 0 || word.compare(QLatin1String("vs."), Qt::CaseInsensitive) == 0)
        return lookup(QStringLiteral("versus"), std::nullopt, ctx);
    if (is({"used", "Used", "USED"}))
        return gold("used", ctx.futureTo ? "VBD" : "DEFAULT");
    return std::nullopt;
}

bool EnglishLexicon::isKnown(const QString& word) const
{
    if (m_golds.contains(word) || m_silvers.contains(word) || word == u'%' || word == u'&' || word == u'+' || word == u'@')
        return true;
    if (!isAlpha(word) || !lexiconChars(word))
        return false;
    if (word.size() == 1)
        return true;
    if (isUpper(word) && m_golds.contains(word.toLower()))
        return true;
    return word.mid(1) == word.mid(1).toUpper();
}

std::optional<QString> EnglishLexicon::stemS(const QString& word, std::optional<double> stress, const Context& ctx) const
{
    if (word.size() < 3 || !word.endsWith(u's'))
        return std::nullopt;
    QString stem;
    if (!word.endsWith(QLatin1String("ss")) && isKnown(word.chopped(1)))
        stem = word.chopped(1);
    else if ((word.endsWith(QLatin1String("'s")) || (word.size() > 4 && word.endsWith(QLatin1String("es")) && !word.endsWith(QLatin1String("ies"))))
             && isKnown(word.chopped(2)))
        stem = word.chopped(2);
    else if (word.size() > 4 && word.endsWith(QLatin1String("ies")) && isKnown(word.chopped(3) + u'y'))
        stem = word.chopped(3) + u'y';
    else
        return std::nullopt;
    return withSuffix(lookup(stem, stress, ctx), suffixS);
}

std::optional<QString> EnglishLexicon::stemEd(const QString& word, std::optional<double> stress, const Context& ctx) const
{
    if (word.size() < 4 || !word.endsWith(u'd'))
        return std::nullopt;
    QString stem;
    if (!word.endsWith(QLatin1String("dd")) && isKnown(word.chopped(1)))
        stem = word.chopped(1);
    else if (word.size() > 4 && word.endsWith(QLatin1String("ed")) && !word.endsWith(QLatin1String("eed")) && isKnown(word.chopped(2)))
        stem = word.chopped(2);
    else
        return std::nullopt;
    return withSuffix(lookup(stem, stress, ctx), suffixEd);
}

std::optional<QString> EnglishLexicon::stemIng(const QString& word, std::optional<double> stress, const Context& ctx) const
{
    static const QRegularExpression doubled(QStringLiteral("([bcdgklmnprstvxz])\\1ing$|cking$"));
    if (word.size() < 5 || !word.endsWith(QLatin1String("ing")))
        return std::nullopt;
    QString stem;
    if (word.size() > 5 && isKnown(word.chopped(3)))
        stem = word.chopped(3);
    else if (isKnown(word.chopped(3) + u'e'))
        stem = word.chopped(3) + u'e';
    else if (word.size() > 5 && doubled.match(word).hasMatch() && isKnown(word.chopped(4)))
        stem = word.chopped(4);
    else
        return std::nullopt;
    return withSuffix(lookup(stem, stress, ctx), suffixIng);
}

std::optional<QString> EnglishLexicon::getWord(QString word, std::optional<double> stress, const Context& ctx) const
{
    if (auto ps = specialCase(word, stress, ctx))
        return ps;
    const QString wl = word.toLower();
    if (word.size() > 1 && isAlpha(QString(word).remove(u'\'')) && word != wl && !m_golds.contains(word) && !m_silvers.contains(word)
        && (isUpper(word) || word.mid(1) == word.mid(1).toLower())
        && (m_golds.contains(wl) || m_silvers.contains(wl) || stemS(wl, stress, ctx) || stemEd(wl, stress, ctx) || stemIng(wl, stress, ctx)))
        word = wl;
    if (isKnown(word))
        return lookup(word, stress, ctx);
    if (word.endsWith(QLatin1String("s'")) && isKnown(word.chopped(2) + QLatin1String("'s")))
        return lookup(word.chopped(2) + QLatin1String("'s"), stress, ctx);
    if (word.endsWith(u'\'') && isKnown(word.chopped(1)))
        return lookup(word.chopped(1), stress, ctx);
    if (auto ps = stemS(word, stress, ctx))
        return ps;
    if (auto ps = stemEd(word, stress, ctx))
        return ps;
    return stemIng(word, stress ? stress : std::optional<double>(0.5), ctx);
}

std::optional<QString> EnglishLexicon::word(const QString& text, const Context& ctx) const
{
    QString w = text;
    w.replace(u'‘', u'\'').replace(u'’', u'\'');
    w = w.normalized(QString::NormalizationForm_KC);
    // Mayúscula inicial o todo en mayúsculas: más acento.
    const std::optional<double> stress = isLower(w) ? std::nullopt : std::optional<double>(isUpper(w) ? 2.0 : 0.5);
    return getWord(w, stress, ctx);
}

QString EnglishLexicon::text(const QString& input, const std::function<QString(const QString&)>& fallback) const
{
    // Palabras (grupos sin espacios, sin la puntuación de los extremos) y signos de puntuación.
    struct Token {
        QStringList parts; // vacío: signo de puntuación
        QString text;
        QString whitespace;
        std::optional<QString> ps;
    };
    static const QString opening = QStringLiteral(u"\"“«(¡¿"), closing = QStringLiteral(u";:,.!?—…\"”»)");
    std::vector<Token> tokens;
    auto punct = [&](QChar c) {
        Token t;
        t.text = c;
        t.ps = c == u'«' ? QStringLiteral(u"“") : c == u'»' ? QStringLiteral(u"”") : c == u'¡' || c == u'¿' ? QString() : QString(c);
        tokens.push_back(std::move(t));
    };
    for (const QString& chunk : input.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        if (chunk == u'-' || chunk == u'–') { // guion suelto: una pausa
            Token t;
            t.text = chunk;
            t.ps = QStringLiteral(u"—");
            tokens.push_back(std::move(t));
            tokens.back().whitespace = QStringLiteral(" ");
            continue;
        }
        qsizetype begin = 0, end = chunk.size();
        while (begin < end && opening.contains(chunk[begin]))
            punct(chunk[begin++]);
        std::vector<QChar> trailing;
        while (end > begin && closing.contains(chunk[end - 1]))
            trailing.insert(trailing.begin(), chunk[--end]);
        if (end > begin) {
            Token t;
            t.text = chunk.mid(begin, end - begin);
            t.parts = subtokenize(t.text);
            if (t.parts.isEmpty())
                t.ps = QString();
            tokens.push_back(std::move(t));
        }
        for (QChar c : trailing)
            punct(c);
        if (!tokens.empty())
            tokens.back().whitespace = QStringLiteral(" ");
    }

    // De derecha a izquierda: cada palabra conoce cómo empieza la siguiente.
    Context ctx;
    for (auto t = tokens.rbegin(); t != tokens.rend(); ++t) {
        if (t->ps) {
            ctx = contextAfter(ctx, t->ps, t->text);
            continue;
        }
        if (t->parts.size() == 1) {
            t->ps = word(t->parts[0], ctx);
            if (!t->ps)
                t->ps = fallback(t->text);
            ctx = contextAfter(ctx, t->ps, t->text);
            continue;
        }
        // Grupo: la parte más larga posible que esté en el léxico, de derecha a izquierda.
        const QStringList& w = t->parts;
        std::vector<std::optional<QString>> ps(size_t(w.size()));
        qsizetype left = 0, right = w.size();
        bool useFallback = false;
        while (left < right) {
            bool blocked = false;
            for (qsizetype i = left; i < right; ++i)
                blocked = blocked || ps[size_t(i)].has_value();
            const QString merged = w.mid(left, right - left).join(QString());
            const std::optional<QString> found = blocked ? std::nullopt : word(merged, ctx);
            if (found) {
                ps[size_t(left)] = found;
                for (qsizetype i = left + 1; i < right; ++i)
                    ps[size_t(i)] = QString();
                ctx = contextAfter(ctx, found, merged);
                right = left;
                left = 0;
            } else if (left + 1 < right) {
                ++left;
            } else {
                --right;
                if (!ps[size_t(right)]) {
                    if (allIn(w[right], kSubtokenJunks)) {
                        ps[size_t(right)] = QString();
                    } else {
                        useFallback = true;
                        break;
                    }
                }
                left = 0;
            }
        }
        t->ps = useFallback ? fallback(t->text) : resolveGroup(w, ps);
    }

    QString out;
    for (const Token& t : tokens)
        out += t.ps.value_or(QString()) + t.whitespace;
    while (out.endsWith(u' '))
        out.chop(1);
    // Kokoro 1.0: la vibrante simple y la oclusión glotal con sus símbolos propios.
    return out.replace(u'ɾ', u'T').replace(u'ʔ', u't');
}

QString englishFromEspeak(QString ps)
{
    // EspeakFallback de misaki: de mayor a menor longitud.
    static const std::pair<const char16_t*, const char16_t*> e2m[] = {
        {u"ʔˌn̩", u"ʔn"}, {u"ʔn̩", u"ʔn"}, {u"a^ɪ", u"I"}, {u"a^ʊ", u"W"}, {u"d^ʒ", u"ʤ"}, {u"e^ɪ", u"A"},
        {u"t^ʃ", u"ʧ"},        {u"ɔ^ɪ", u"Y"},       {u"ə^l", u"ᵊl"}, {u"ʲo", u"jo"},  {u"ʲə", u"jə"},  {u"e", u"A"},
        {u"ʲ", u""},           {u"ɚ", u"əɹ"},        {u"r", u"ɹ"},    {u"x", u"k"},    {u"ç", u"k"},    {u"ɐ", u"ə"},
        {u"ɬ", u"l"},          {u"̃", u""},
    };
    for (const auto& [from, to] : e2m)
        ps.replace(QString::fromUtf16(from), QString::fromUtf16(to));
    static const QRegularExpression syllabic(QStringLiteral(u"(\\S)̩"));
    ps.replace(syllabic, QStringLiteral(u"ᵊ\\1"));
    ps.remove(QChar(0x0329));
    ps.replace(QStringLiteral(u"o^ʊ"), QStringLiteral(u"O"));
    ps.replace(QStringLiteral(u"ɜːɹ"), QStringLiteral(u"ɜɹ"));
    ps.replace(QStringLiteral(u"ɜː"), QStringLiteral(u"ɜɹ"));
    ps.replace(QStringLiteral(u"ɪə"), QStringLiteral(u"iə"));
    ps.remove(u'ː');
    ps.replace(u'o', u'ɔ');
    ps.replace(u'ɾ', u'T').replace(u'ʔ', u't');
    return ps.remove(u'^');
}

} // namespace movix::ai
