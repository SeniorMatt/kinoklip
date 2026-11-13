// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QTranslator>

#include <memory>

// Global scope on purpose (Q_INIT_RESOURCE, see ui/common/Resources.cpp).
static void initTranslationResources()
{
#ifdef KK_HAVE_TRANSLATIONS
    Q_INIT_RESOURCE(translations);
#endif
}

namespace kk::i18n {

namespace {

/// The application's translations with a second chance: a text that is not found under the
/// context the code asked for is looked up in "KinoklipAny" (scripts/ts_fallback.py adds it
/// at build time). Many widgets have no Q_OBJECT or forward to another class's tr(), so the
/// context lupdate recorded is not the one used at run time.
class FallbackTranslator : public QTranslator
{
public:
    using QTranslator::QTranslator;
    QString translate(const char* context, const char* sourceText, const char* disambiguation,
        int n) const override
    {
        QString result = QTranslator::translate(context, sourceText, disambiguation, n);
        if (result.isNull() && !disambiguation)
            result = QTranslator::translate("KinoklipAny", sourceText, nullptr, n);
        return result;
    }
};

FallbackTranslator* g_app = nullptr;
QTranslator* g_qt = nullptr;

bool known(const QString& code)
{
    for (const Language& l : languages())
        if (l.code == code)
            return true;
    return false;
}

QString baseLanguage(const QString& name)
{
    return name.toLower().section(QRegularExpression(QStringLiteral("[-_.@]")), 0, 0);
}

} // namespace

QList<Language> languages()
{
    // Each name in its own language, never translated (a user who cannot read the current
    // language must still find theirs).
    return {{QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("de"), QStringLiteral("Deutsch")},
        {QStringLiteral("es"), QStringLiteral("Español")},
        {QStringLiteral("ru"), QStringLiteral("Русский")},
        {QStringLiteral("uk"),
            QStringLiteral("Українська")}};
}

QString resolveLanguage(const QString& commandLine)
{
    QString wanted = commandLine.trimmed();
    if (wanted.isEmpty())
        wanted = QString::fromUtf8(qgetenv("KK_LANG")).trimmed();
    if (wanted.isEmpty())
        wanted
            = QSettings().value(QLatin1String(kLanguageKey), QStringLiteral("system")).toString();
    wanted = baseLanguage(wanted);
    if (known(wanted))
        return wanted;
    // "system" or anything unknown: the desktop's UI languages in order of preference.
    for (const QString& name : QLocale::system().uiLanguages()) {
        const QString code = baseLanguage(name);
        if (known(code))
            return code;
    }
    return QStringLiteral("en");
}

void installLanguage(const QString& code)
{
    initTranslationResources();
    if (g_app) {
        QCoreApplication::removeTranslator(g_app);
        delete g_app;
        g_app = nullptr;
    }
    if (g_qt) {
        QCoreApplication::removeTranslator(g_qt);
        delete g_qt;
        g_qt = nullptr;
    }
    if (code.isEmpty() || code == QLatin1String("en"))
        return;
    auto app = std::make_unique<FallbackTranslator>();
    if (app->load(QStringLiteral(":/translations/kinoklip_%1.qm").arg(code))) {
        g_app = app.release();
        QCoreApplication::installTranslator(g_app);
    }
    // Qt's own strings: the Qt install's folder, then the AppImage's copy.
    const QStringList folders{QLibraryInfo::path(QLibraryInfo::TranslationsPath),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/kinoklip/translations")};
    auto qt = std::make_unique<QTranslator>();
    for (const QString& folder : folders) {
        if (qt->load(QStringLiteral("qtbase_%1").arg(code), folder)) {
            g_qt = qt.release();
            QCoreApplication::installTranslator(g_qt);
            break;
        }
    }
}

QString install(const QString& commandLine)
{
    const QString code = resolveLanguage(commandLine);
    installLanguage(code);
    return code;
}

} // namespace kk::i18n
