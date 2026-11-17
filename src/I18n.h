#pragma once
// The user-facing text of the app in the user's language.
//
// The English text is the key: T("Open File...") returns the translation for the current
// language, or the English text itself when there is none (so a missing translation shows
// English, never an empty label). Translations live in resources/lang/<code>.json as
// {"English text": "translation"} and are built into the program; "@name" in a file is the
// language's own name for itself ("Türkçe"), shown in the language menu.
// Sentences with a value in them use {} placeholders, so each language can put the value
// where its grammar wants it: TF("Playing on {}", {name}). A translation that needs the values
// in another order numbers them: "{2}年{1}{0}日" for "{0} {1} {2}".

#include <initializer_list>
#include <string>
#include <vector>

namespace i18n {

// Picks the language: TOLGA_LANG if set ("tr", "en"), else the one chosen in the app,
// else the system's preferred one. Call once after SDL_Init, before any text is drawn.
void init();

// "en", "tr"... the language in use.
const std::string& language();

struct Language { std::string code, name; };
// English and every built-in translation, in the order of their codes.
const std::vector<Language>& languages();

// What the user chose in the app: a code, or "" for the system's language.
const std::string& choice();
// Switches the language now and remembers the choice ("" = follow the system).
// Call from the UI thread; text already shown elsewhere stays as it is.
void choose(const std::string& code);

}  // namespace i18n

const char* T(const char* english);
std::string TF(const char* english, std::initializer_list<std::string> values);
