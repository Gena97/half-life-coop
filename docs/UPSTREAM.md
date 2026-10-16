# Исходная база

Проект основан на официальном [Half-Life SDK от Valve](https://github.com/ValveSoftware/halflife)

- Исходный commit: `b1b5cf5892918535619b2937bb927e46cb097ba1`
- Исходный remote: `upstream`
- Лицензия SDK сохранена в [LICENSE](../LICENSE)
- Исходный README Valve сохранён в [HLSDK_README.md](HLSDK_README.md)

Код клиента и сервера, библиотеки для сборки и исходные проекты Visual Studio взяты из SDK. Кооперативные изменения находятся в `dlls/`. Исходный код Sven Co-op не использовался

Лицензия Valve предусматривает бесплатное распространение модификаций SDK. Ресурсы оригинальной Half-Life не входят в пакет этого проекта: при запуске мод использует установленную игру через `fallback_dir valve`
