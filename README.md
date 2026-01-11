Вариант:
- копирующая сборка мусора (используя semi-DFS подход)
- инкрементальная сборка

## Сборка

`git clone git@github.com:deevroman/stella-gc.git && cd stella-gc`

В корне проекта должен лежать main.c со скомпилированной Stella-программой. Создать такой файл можно так:

`./bin/stella-linux compile < examples/exp-many-writes.stella > main.c`

Собирается с помощью CMake:

```bash
rm -rf build/*
cmake -B build -DMAX_ALLOC_SIZE="1024*1024*2"
cmake --build build
```

Запуск: `./build/stella`

Доступные флаги сборки:

- `-DSTELLA_LOGS=yes` — полные логи Stella
- `-DGC_STATES=yes` — выводить print_gc_state() в начале и в конце сборки мусора
- `-DSANITIZE=yes` — собрать с санитайзерами
- `-DGC_DEBUG=yes` — включить логи и проверки целостности
- `-DEPSILON_GC=yes` — использовать malloc() без free() в качестве сборщика мусора

В случае нехватки памяти программа завершается с кодом 42.

---

## Пояснения

В целом вся суть выбранного варианта сборщика мусора находится в функции gc_step (stella/gc.c)
Она принимает количество байт, на которые она может продвинуть сборку мусора, и в ней реализован semi-dfs

Однако случайно я усложнил (или так и нужно было) себе задачу, восприняв MAX_ALLOC_SIZE, как общий лимит на количество аллоцированной памяти с учётом корней.

Так как количетсво корней программы не известно сборщику мусора заранее, я храню список корней вместе с остальными объектами. 

Это немного усложняет запуск сборки (нужно копировать корни) и потребовало барьера на запись.
Как я понял, это происходит потому что компилятор Stella не ожидает, что gc_push_root может стриггерить сборку мусора.
И это приводит к поломке некоторых программ.

<details>
<summary>Воспроизвести можно так:</summary>

```bash
./bin/stella-macos compile < examples/exp-many-writes.stella > main.c
rm -rf build/*
cmake -B build -DMAX_ALLOC_SIZE='1024*1024*1' -DOFF_WRITE_BARRIER=yes
cmake --build build
echo 11 | ./build/stella
```

Результат 2047 ошибочен. Включение барьера на запись помогает поймать запись во from-space при сборке мусора.
</details>

## Пример запуска и вывода

```bash
./bin/stella-macos compile < examples/exp-many-writes.stella > main.c
rm -rf build/*
cmake -B build -DMAX_ALLOC_SIZE='65536' -DGC_STATES=yes
cmake --build build
echo 8 | ./build/stella
```

Результат:
```
GC state:                         <- print_gc_state() в начале сборки мусора
  collecting: yes
  from-space used (with headers): 0 bytes
  to-space used (with headers):   5763024896 bytes, scanned: 0 bytes
  Number of roots on stack: 33
  Current allocated bytes (payload): 16760
  Current allocated objects: 1000
GC state:                         <- print_gc_state() в конце сборки мусора
  collecting: no
  current space used (with headers): 24064 bytes of 32768
  Number of roots on stack: 28    <- часть корней успели попнуть
  Current allocated bytes (payload): 2032
  Current allocated objects: 125  <- мусор почистили
GC state:                         <- print_gc_state() в начале второй сборки мусора
  collecting: yes
  from-space used (with headers): 0 bytes
  to-space used (with headers):   5763061696 bytes, scanned: 0 bytes
  Number of roots on stack: 31
  Current allocated bytes (payload): 12256
  Current allocated objects: 736
GC state:                         <- print_gc_state() в конце второй сборки мусора
  collecting: no
  current space used (with headers): 16528 bytes of 32768
  Number of roots on stack: 28
  Current allocated bytes (payload): 3240
  Current allocated objects: 201
GC state:                         <- print_gc_state() третья сборка началась, но программа завершилась быстрее
  collecting: yes
  from-space used (with headers): 0 bytes
  to-space used (with headers):   5763031352 bytes, scanned: 0 bytes
  Number of roots on stack: 31
  Current allocated bytes (payload): 8368
  Current allocated objects: 508
256

------------------------------------------------------------   <- вывод print_gc_alloc_stats()
Garbage collector (GC) statistics:
  Total allocated bytes: 49416
  Total allocated objects: 2952
  Current allocated bytes: 12920
  Current allocated objects: 780
  Maximum residency bytes: 21208
  Maximum residency objects: 1266
  Read operations: 1839
  Write operations: 255
  Read barrier triggers: 65
  Write barrier triggers: 1
  GC cycles: 3

------------------------------------------------------------
Stella runtime statistics:
Total allocated fields in Stella objects: 811 fields
```

--- 

## Задание

#### Интерфейс сборщика 

Реализация сборщика мусора должна удовлетворять как минимум интерфейсу, описанному в файле gc.h:
1. макрос GC_READ_BARRIER(object, field_index, read_code) должен быть определён для раскрытия чтения поля объекта; если барьера на чтение нет, достаточно раскрыть макрос в read_code;
2. макрос GC_WRITE_BARRIER(object, field_index, contents, write_code) должен быть определён для раскрытия записи поля объекта; если барьера на запись нет, достаточно раскрыть макрос в write_code;
3. процедура void* gc_alloc(size_t size_in_bytes) должна реализовывать механизм выделения блока памяти, размером как минимум size_in_bytes байт;
4. процедура void gc_read_barrier(void *object, int field_index) должна реализовывать механизм барьера на чтение;
5. процедура void gc_write_barrier(void *object, int field_index, void *contents) должна реализовывать механизм барьера на запись;
6. процедура gc_push_root(void **object) добавляет ссылку на новый корень программы в множество отслеживаемых корней;
7. процедура gc_pop_root(void **object) убирает ссылку на корень программы из множество отслеживаемых корней;
8. процедура void print_gc_alloc_stats() печатает статистику использования сборщика мусора на момент вызова;
9. процедура void print_gc_state() печатает состояние сборщика мусора на момент вызова;
   
10. Каждый алгоритм может предполагать ряд параметров, которые представлены макросами, передающимися во время компиляции модуля сборки мусора. В этом проекте достаточно реализовать один параметр:
  - MAX_ALLOC_SIZE — максимальный размер памяти для кучи (на молодое поколение) в байтах.

#### Статистика сборщика

Сборщик мусора должен собирать статистику работы и печатать её при вызове процедуры print_gc_alloc_stats()

Информация может быть распечатана в произвольном формате, но должна содержать:
1. Общее количество выделенной памяти (в байтах и в блоках/объектах)
2. Общее количество сборок мусора, всего и на каждое поколение
3. Максимальное количество используемой памяти (выделенной, но ещё не собранной) всего и по каждому поколению
4. Количество чтений и записей в памяти (контролируемой сборщиком)
5. Количество срабатываний барьера на чтение/запись

При сборке можно указывать флаги, влияющие на отладочную печать и печать статистики среды исполнения:

   • STELLA_DEBUG — включить отладочную печать

   • STELLA_GC_STATS — печатать статистику работы сборщика мусора при завершении программы

   • STELLA_RUNTIME_STATS — печатать статистику работы среды времени исполнения Stella при завершении программы