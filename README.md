# Отчет: Изолированный запуск процесса

## Подготовка

Работа была выполнена в ОС Linux Ubuntu. В качестве ОС для *"контейнера"* использовалась Linux Alpine. Для установки выполнил следующий набор команд:

```bash
mkdir -p /tmp/alpine
cd /tmp/alpine
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.0-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.19.0-x86_64.tar.gz
rm alpine-minirootfs-3.19.0-x86_64.tar.gz
```

В файле [runtime.c](/src/runtime.c) была реализована простая программа запуска произвольной команды внутри минимально изолированного окружения.

Для сборки выполнил:

```bash
gcc -Wall -o runtime runtime.c
```

Пример запуска программы:

```bash
./runtime run --rootfs /tmp/alpine /bin/ls
```

## О программе

Программу можно разделить на две логические части:
1. Родительский процесс
    - Создаёт контейнер и настраивает его пользовательское пространство имён.

2. Дочерний процесс
    - Непосредственно находится внутри создаваемого контейнера, настраивает файловую систему, монтирует служебные файловые системы и запускает целевую команду.

При этом Дочерний процесс ждет сигнал pipe от родителя о готовности пространства имен.

## Результаты

### Тест 1: Вывод файлов `/bin/ls`

Запуск на хосте:

- Команда:
```bash
ls
```

- Вывод:
```bash
bin   dev  home  lib    lost+found  mnt  proc  run   snap  sys  usr
boot  etc  init  lib64  media       opt  root  sbin  srv   tmp  var
```

Запуск в контейнере:

- Команда:
```bash
./runtime run --rootfs /tmp/alpine /bin/ls
```

- Вывод:
```bash
bin    etc    lib    mnt    proc   run    srv    tmp    var
dev    home   media  opt    root   sbin   sys    usr
```

*Явно видно, что запуск `ls` в контейнере вывел содержимое Linux Alpine.*

### Тест 2: Проверка PID `/bin/ps`

Запуск на хосте:

- Команда:
```bash
ps
```

- Вывод:
```bash
    PID TTY          TIME CMD
    402 pts/0    00:00:00 bash
   6846 pts/0    00:00:00 ps
```

Запуск в контейнере:

- Команда:
```bash
./runtime run --rootfs /tmp/alpine /bin/ps
```


- Вывод:
```bash
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/ps
```

*Дочерний процесс видит себя под PID 1, что подтверждает работу CLONE_NEWPID и корректное монтирование /proc. Также видно пользователя root в дочернем процессе.*

### Тест 3: Анализ изоляций

Запуск на хосте:

- Команда:
```bash
id
hostname
```

- Вывод:
```bash
uid=1000(serme) gid=1000(serme) groups=1000(serme),4(adm),24(cdrom),27(sudo),30(dip),46(plugdev),100(users)
PC-0
```

*Пользователь - мой созданный с `uid = 1000`. Тажке `hostname = PC-0`, я так и задавал.*

Запуск в контейнере:

- Команда:
```bash
./runtime run --rootfs /tmp/alpine /usr/bin/id
./runtime run --rootfs /tmp/alpine /bin/hostname
```

- Вывод:
```bash
uid=0(root) gid=0(root) groups=65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),0(root)
mini-container
```

*Пользователь - `uid = 0 (root)`, а `hostname = mini-container`, я так и задавал в программе.*

**Вывод: Благодаря `CLONE_NEWUSER` непривилегированный пользователь маппится в root (UID 0) внутри контейнера, что позволяет выполнять chroot и mount. Благодаря `CLONE_NEWUTS` мы можем безопасно изменить hostname внутри контейнера, не затрагивая хостовую машину.**

### Тест 4: смонтировать `/proc` без изоляции mount-пространства

Запустим модифиуированную программу [runtime_no_mount.c](/src/runtime_no_mount.c), в которой отсутствует флаг `CLONE_NEWNS`. Соберем:

```bash
gcc -Wall -o runtime_no_mount runtime_no_mount.c
```

Запуск в контейнере:

- Команда:
```bash
./runtime_no_mount run --rootfs /tmp/alpine /usr/bin/id
```

- Вывод:
```bash
Ошибка монтирования /proc: Operation not permitted
Ошибка монтирования /dev: Operation not permitted
uid=0(root) gid=0(root) groups=65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),0(root)
```

**Вывод: Произошла ошибка прав доступа. Пространства имен пользователей (User Namespaces) выдают мандаты только в рамках нового пространства имен монтирования. Если мы остаемся в mount-пространстве хоста, ядро видит, что у  пользователя нет реальных прав root в глобальной таблице монтирования. Отсюда `CLONE_NEWNS` обязателен, потому что он создает изолированную таблицу монтирования, в которой User Namespace получает CAP_SYS_ADMIN, позволяя безопасно монтировать proc для своего изолированного PID-неймспейса без влияния на хост.**
