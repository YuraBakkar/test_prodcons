# Producer

Консольний застосунок на C++17, який читає розмір корисного навантаження
пакета з командного рядка. Розмір задається додатним цілим числом у байтах.

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

Результат:

```text
Payload size: 1024 bytes
```
