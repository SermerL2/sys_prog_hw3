#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define STACK_SIZE (1024 * 1024) // 1 MB стек для дочернего процесса

struct container_args {
    int pipe_fd;      // Дескриптор чтения из pipe для синхронизации
    char *rootfs;     // Путь к корневой ФС
    char **cmd;       // Команда для запуска
};

// Вспомогательная функция для записи в файлы /proc
void write_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Ошибка открытия %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (write(fd, content, strlen(content)) < 0) {
        fprintf(stderr, "Ошибка записи в %s: %s\n", path, strerror(errno));
        exit(1);
    }
    close(fd);
}

// Функция, выполняемая в дочернем процессе (внутри контейнера)
int child_func(void *arg) {
    struct container_args *cargs = (struct container_args *)arg;
    char ch;

    // 1. Ожидаем сигнала от родителя, что UID/GID маппинг настроен
    read(cargs->pipe_fd, &ch, 1);
    close(cargs->pipe_fd);

    // 2. Настраиваем UTS (hostname)
    sethostname("mini-container", 14);

    // 3. Создаем точки монтирования (если их нет)
    char path[512];
    snprintf(path, sizeof(path), "%s/proc", cargs->rootfs);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/dev", cargs->rootfs);
    mkdir(path, 0755);

    // 4. Монтируем служебные ФС
    snprintf(path, sizeof(path), "%s/proc", cargs->rootfs);
    if (mount("proc", path, "proc", 0, NULL) != 0) {
        perror("Ошибка монтирования /proc");
    }

    snprintf(path, sizeof(path), "%s/dev", cargs->rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) != 0) {
        perror("Ошибка монтирования /dev");
    }

    // 5. Изоляция файловой системы (chroot)
    if (chroot(cargs->rootfs) != 0) {
        perror("Ошибка chroot");
        exit(1);
    }
    chdir("/");

    // 6. Заменяем процесс на целевую команду
    execvp(cargs->cmd[0], cargs->cmd);
    perror("Ошибка execvp");
    exit(1);
}

int main(int argc, char **argv) {
    // Парсинг аргументов
    if (argc < 5 || strcmp(argv[1], "run") != 0 || strcmp(argv[2], "--rootfs") != 0) {
        fprintf(stderr, "Использование: %s run --rootfs <path> <command> [args...]\n", argv[0]);
        return 1;
    }

    char *rootfs_path = argv[3];
    char **cmd = &argv[4];

    // Создаем pipe для синхронизации
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        perror("pipe");
        return 1;
    }

    // Память стека дочернего процесса
    char *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    char *stack_top = stack + STACK_SIZE; // стек растет вниз

    struct container_args cargs;
    cargs.pipe_fd = pipe_fd[0]; // Read end
    cargs.rootfs = rootfs_path;
    cargs.cmd = cmd;

    // Флаги для изоляции всех требуемых пространств имен
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER |
                CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD; // как в задании

    // Создаем изолированный процесс
    pid_t child_pid = clone(child_func, stack_top, flags, &cargs);
    if (child_pid < 0) {
        perror("clone");
        return 1;
    }

    // РОДИТЕЛЬСКИЙ ПРОЦЕСС
    close(pipe_fd[0]); // закрываем чтение

    char map_path[256];
    char map_content[64];

    // Маппинг UID: 0 (внутри) -> getuid() (снаружи)
    snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", child_pid);
    snprintf(map_content, sizeof(map_content), "0 %d 1\n", getuid());
    write_file(map_path, map_content);

    // Отключаем setgroups (требуется ядром для непривилегированных gid_map)
    snprintf(map_path, sizeof(map_path), "/proc/%d/setgroups", child_pid);
    write_file(map_path, "deny\n");

    // Маппинг GID: 0 (внутри) -> getgid() (снаружи)
    snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", child_pid);
    snprintf(map_content, sizeof(map_content), "0 %d 1\n", getgid());
    write_file(map_path, map_content);

    // Отправляем сигнал дочернему процессу, что можно продолжать
    write(pipe_fd[1], "1", 1);
    close(pipe_fd[1]);

    // Ждем завершения контейнера
    waitpid(child_pid, NULL, 0);
    
    // Освобождаем стек
    munmap(stack, STACK_SIZE);
    return 0;
}