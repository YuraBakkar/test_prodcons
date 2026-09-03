# Producer

Консольний застосунок на C++17, який безперервно створює пакети із випадковим
payload заданого розміру. Кожен пакет містить Unix timestamp у наносекундах,
sequence number, розмір payload і контрольну суму CRC-32.

Пакети передаються Consumer через POSIX shared memory
`/test_prodcons_packets`. Для синхронізації використовується кільцевий буфер
із process-shared семафорами: Producer засинає, якщо Consumer не встигає
звільняти слоти. Розмір shared memory обмежено приблизно 64 MiB, а payload —
16 MiB.

## Збірка

```bash
cmake -S . -B build
cmake --build build
```

## Запуск

```bash
./build/producer <payload_size_bytes>
```

Наприклад:

```bash
./build/producer 1024
```

Producer працює до натискання `Ctrl+C`. Без запущеного Consumer він заповнить
кільцевий буфер і чекатиме звільнення слотів.

Приклад початкового повідомлення:

```text
Producing 1024-byte packets in /test_prodcons_packets (256 slots). Press Ctrl+C to stop.
```
