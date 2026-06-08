#include <Windows.h>
#include "NPP/Scintilla.h"
#include "NPP/PluginInterface.h"
#include "Highlighting.h"
#include "TagsDatabase.h"
#include "NppTags.h"
#include <string>
#include <atomic>

enum SymbolType {
	SYM_FUNCTION,
	SYM_CLASS,
	SYM_VARIABLE,
	SYM_ENVIRONMENT,
	SYM_COUNT,
	SYM_NONE = -1
};

static int g_indicator_start = -1;

// Фолбэк на случай старых версий Notepad++ (< 8.5.6)
#ifndef NPPM_ALLOCATEINDICATOR
#define NPPM_ALLOCATEINDICATOR (NPPMSG + 113)
#endif

#ifndef INDIC_TEXTFORE
#define INDIC_TEXTFORE 17 // Стиль индикатора, меняющий цвет текста
#endif

std::atomic_flag stmt_mutex = ATOMIC_FLAG_INIT;
SqliteStatement* stmt = nullptr;

SymbolType getSymbolType(std::string s)
{
	if (stmt_mutex.test_and_set(std::memory_order_acquire))
		return SymbolType::SYM_NONE;
	if (stmt == nullptr)
	{
		stmt_mutex.clear();
		return SymbolType::SYM_NONE;
	}
	SymbolType st = SymbolType::SYM_NONE;
	stmt->Bind("@tag", s.c_str());
	while (stmt->GetNextRecord())
	{
		std::string tp = stmt->GetTextColumn("Type");
		if (tp == "class" || tp == "enum" || tp == "union" || tp == "typedef" || tp == "struct")
			st = SymbolType::SYM_CLASS;
		else if (tp == "function")
			st = SymbolType::SYM_FUNCTION;
		else if (tp == "variable")
			st = SymbolType::SYM_VARIABLE;
	}
	stmt->Reset();
	stmt_mutex.clear(std::memory_order_release);
	return st;
}

inline bool isIdentifierChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void initIndicators();

void initHighlighting()
{
	initIndicators();
}

void subinit()
{
	while (stmt_mutex.test_and_set());
	try
	{
		g_DB->Open();
		stmt = new SqliteStatement(g_DB, "SELECT Type FROM Tags WHERE Tag = @tag LIMIT 1;");
		initIndicators();
		stmt_mutex.clear();
	}
	catch (...)
	{
		stmt = nullptr;
		stmt_mutex.clear(std::memory_order_release);
	}
}

void subreset()
{
	while (stmt_mutex.test_and_set());
	delete stmt;
	stmt = NULL;
	g_DB->Close();
	stmt_mutex.clear(std::memory_order_release);
}

void resetHighlighting()
{
}

// Инициализация индикаторов (вызывать один раз при старте плагина)
void initIndicators() {
	if (g_indicator_start != -1) return;

	// Запрашиваем 4 индикатора у Notepad++
	::SendMessage(g_nppData._nppHandle, NPPM_ALLOCATEINDICATOR, SYM_COUNT, (LPARAM)&g_indicator_start);

	// Цвета (COLORREF имеет формат BGR)
	COLORREF colors[SYM_COUNT] = {
		RGB(116, 83, 31),   // Function - Синий
		RGB(43, 145, 175),    // Class/Struct/Union - Зеленый
		RGB(112, 128, 144),    // Variable - Оранжевый
		RGB(143, 8, 196)        // Defines - Коричневый
	};

	// Инициализируем стили для обеих панелей Scintilla (основной и вторичной)
	HWND scintillas[2] = { g_nppData._scintillaMainHandle, g_nppData._scintillaSecondHandle };
	for (int v = 0; v < 2; v++) {
		for (int i = 0; i < SYM_COUNT; i++) {
			int ind = g_indicator_start + i;
			::SendMessage(scintillas[v], SCI_INDICSETSTYLE, ind, INDIC_TEXTFORE);
			::SendMessage(scintillas[v], SCI_INDICSETFORE, ind, colors[i]);
		}
	}
}

HWND getCurrentScintilla() {
	int which = 0;
	::SendMessage(g_nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
	return (which == 0) ? g_nppData._scintillaMainHandle : g_nppData._scintillaSecondHandle;
}

// Основная функция применения подсветки
void applyHighlighting() {
	subinit();
	HWND curScintilla = getCurrentScintilla();

	// 1. Определяем видимый участокок кода (строки)
	int firstVisibleLine = ::SendMessage(curScintilla, SCI_GETFIRSTVISIBLELINE, 0, 0);
	int linesOnScreen = ::SendMessage(curScintilla, SCI_LINESONSCREEN, 0, 0);
	int lastVisibleLine = firstVisibleLine + linesOnScreen;

	int totalLines = ::SendMessage(curScintilla, SCI_GETLINECOUNT, 0, 0);
	if (lastVisibleLine >= totalLines) lastVisibleLine = totalLines - 1;

	int startPos = ::SendMessage(curScintilla, SCI_POSITIONFROMLINE, firstVisibleLine, 0);
	int endPos = ::SendMessage(curScintilla, SCI_GETLINEENDPOSITION, lastVisibleLine, 0);

	if (endPos <= startPos) return;

	// 2. Очищаем старые индикаторы СТРОГО в видимой области
	// Это критически важно: Scintilla сама хранит индикаторы для уже проскролленных зон.
	for (int i = 0; i < SYM_COUNT; i++) {
		::SendMessage(curScintilla, SCI_SETINDICATORCURRENT, g_indicator_start + i, 0);
		::SendMessage(curScintilla, SCI_INDICATORCLEARRANGE, startPos, endPos - startPos);
	}

	// 3. Вытягиваем текст видимой области
	Sci_TextRange tr;
	tr.chrg.cpMin = startPos;
	tr.chrg.cpMax = endPos;
	std::string text(endPos - startPos + 1, '\0');
	tr.lpstrText = &text[0];
	::SendMessage(curScintilla, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);

	// 4. Парсим идентификаторы и применяем стили
	int len = endPos - startPos;
	int i = 0;
	while (i < len) {
		while (i < len && !isIdentifierChar(text[i])) {
			i++;
		}
		if (i >= len) break;

		int wordStart = i;
		while (i < len && isIdentifierChar(text[i])) {
			i++;
		}
		int wordLen = i - wordStart;

		// Игнорируем числа (идентификатор не может начинаться с цифры)
		if (text[wordStart] >= '0' && text[wordStart] <= '9') continue;

		std::string word = text.substr(wordStart, wordLen);
		int type = getSymbolType(word);

		if (type >= 0 && type < SYM_COUNT) {
			::SendMessage(curScintilla, SCI_SETINDICATORCURRENT, g_indicator_start + type, 0);
			::SendMessage(curScintilla, SCI_INDICATORFILLRANGE, startPos + wordStart, wordLen);
		}
	}
	subreset();
}
