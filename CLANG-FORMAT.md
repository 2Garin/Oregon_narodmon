# clang-format

В проекте лежит файл `.clang-format` — единый стиль оформления C/C++ кода (аналог PSR в мире PHP).
Утилита `clang-format` читает этот файл и приводит исходники к описанному в нём стилю автоматически.

## Установка

```bash
sudo apt install clang-format
```

## Использование из командной строки

```bash
# Посмотреть, что clang-format хочет изменить (без правок файла):
clang-format --dry-run --Werror Oregon_narodmon.ino

# Показать diff:
clang-format Oregon_narodmon.ino | diff -u Oregon_narodmon.ino -

# Применить форматирование к файлу:
clang-format -i Oregon_narodmon.ino
```

> Нюанс: clang-format может не распознать `.ino` как C++ автоматически (зависит от версии).
> Если ругается на расширение — добавьте флаг `--assume-filename=Oregon_narodmon.cpp`.

## IDE-интеграция

- **Arduino IDE 2.x** подхватывает `.clang-format` автоматически — формат по `Ctrl+Shift+F`.
- **VSCode** + расширение «Arduino» или «C/C++» — `Shift+Alt+F` или автоматически по `Ctrl+S`,
  если включено `editor.formatOnSave`.
- **CLion** — File → Settings → Editor → Code Style → C/C++ → ClangFormat.

## Что выбрано в конфиге и почему

| Настройка | Значение | Зачем |
|---|---|---|
| `BasedOnStyle: Google` | — | разумный дефолт, близок к существующему коду |
| `IndentWidth: 2` | 2 пробела | как в текущем коде |
| `ColumnLimit: 120` | 120 | в коде длинные строки `#define` с комментариями, 80 будет их рвать |
| `BreakBeforeBraces: Custom` | гибрид | `void setup() {` на новой строке для функций/struct, K&R для `if/else/циклов` (как сейчас) |
| `AllowShortIfStatementsOnASingleLine: WithoutElse` | да | `if (TEST_MODE) Serial.println(...)` остаётся одной строкой |
| `SortIncludes: false` | не сортировать | для embedded порядок `#include` иногда важен (например, `Arduino.h` должен быть первым) |
| `ReflowComments: false` | не трогать | сохраняет баннеры `//////` без переформатирования |
| `PointerAlignment: Left` | `int* p` | единый стиль для указателей |
| `KeepEmptyLinesAtTheStartOfBlocks: false` | да | убирает пустые строки сразу после `{` |
| `MaxEmptyLinesToKeep: 1` | 1 | не больше одной пустой строки подряд |

## Полная документация

Все доступные опции и их значения — на сайте LLVM:
<https://clang.llvm.org/docs/ClangFormatStyleOptions.html>
