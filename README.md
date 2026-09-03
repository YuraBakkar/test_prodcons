# Producer / Consumer

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

Producer працює до натискання `Ctrl+C`. Керування передачею:

- будь-яка клавіша в терміналі — перемкнути pause/resume;
- `kill -USR1 <pid>` — призупинити;
- `kill -USR2 <pid>` — відновити;
- `kill -TERM <pid>` або `Ctrl+C` — завершити.

Без запущеного Consumer Producer заповнить кільцевий буфер і чекатиме
звільнення слотів.

Приклад початкового повідомлення:

```text
Producing 1024-byte packets in /test_prodcons_packets (256 slots).
SIGUSR1 pauses, SIGUSR2 resumes, Ctrl+C stops. Any key toggles pause/resume.
```

## Consumer

Consumer можна запускати до або після Producer в іншому терміналі:

```bash
./build/consumer
```

Якщо Producer ще не запущений, Consumer чекатиме на появу shared memory без
активного опитування. Під час очікування працюють сигнали завершення та
pause/resume.

Він читає пакети зі shared memory та перевіряє timestamp, розмір payload,
послідовність номерів і CRC-32. Приблизно раз на секунду виводиться:

- загальна кількість отриманих пакетів (`total`);
- кількість пакетів за секунду (`packets/s`);
- кількість байтів payload за секунду (`bytes/s`);
- кількість помилок checksum, метаданих і послідовності.

Приклад:

```text
total=40000 packets/s=39999.8 bytes/s=40959829.9 checksum_errors=0 metadata_errors=0 sequence_errors=0
```

Consumer підтримує те саме керування: будь-яка клавіша перемикає pause/resume,
`SIGUSR1` призупиняє, `SIGUSR2` відновлює, а `Ctrl+C` або `SIGTERM` завершує.

Одночасно для поточного користувача може працювати лише один Consumer. Він
утримує advisory lock у `/tmp/test_prodcons_consumer_<uid>.lock`; спроба
запустити другий екземпляр завершується повідомленням
`Another Consumer is already running.`
