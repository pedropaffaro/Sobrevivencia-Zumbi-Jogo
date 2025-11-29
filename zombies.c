#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <semaphore.h>
#include <stdatomic.h>

/* Constantes */
#define VEL_ZUMBI 350000
#define ALTURA 10
#define LARGURA 20
#define NUM_ZUMBIS 6

/* Struct para zumbis */
typedef struct
{
    int x, y; // Posição do zumbi
} Zumbi;

Zumbi zumbis[NUM_ZUMBIS];

/* Declaração antecipada */
void *thread_power_timer(void *arg);

/* Estado compartilhado */
// Mapa de caracteres representando o mundo do jogo
// . = vazio
// J = jogador
// Z = zumbi
// P = power up
char mapa[ALTURA][LARGURA];

// Posição inicial do jogador (centro do mapa)
// Essas variáveis serão lidas por várias threads mais a frente
int jogador_x = ALTURA / 2;
int jogador_y = LARGURA / 2;

// Flag de término do jogo (também acessada por várias threads)
// Protegida por sem_state quando necessário
int game_over = 0;

/* Semáforos */
// sem_mapa: garante exclusão mútua do mapa
// sem_spawn: protege o estado power up (power_ativo_mapa)
// sem_power: controla a ativação do power up pra evitar stacking
// sem_state: protege a flag game_over e outras leituras sensíveis
sem_t *sem_mapa;
sem_t *sem_spawn;
sem_t *sem_power;
sem_t *sem_state;

/* Power up */
int power_x = -1, power_y = -1; // Posição do power up no mapa
int power_ativo_mapa = 0;       // Indica se há um power up no mapa (máximo de 1)
int invencivel = 0;             // Flag de efeito do power up (1 = invencível)

/* Pontuação */
// atomic_int garante que incrementos/leitura sejam seguros, sem a necessidade de
// semáforo apenas para a pontuação
atomic_int pontos = 0;

/* FUNÇÕES AUXILIARES */
/* Limpa a tela */
void limpar_tela()
{
    printf("\033[H\033[J");
}

/*
Lê teclado sem bloqueio (não precisa dar enter)
Retorna 1 se há tecla pronta, 0 caso contrário.
*/
int kbhit(void)
{
    struct termios oldt, newt;
    int ch, oldf;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF)
    {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}

/* FUNÇÕES DO MAPA */
/* Inicializa o mapa (chamar antes de criar threads) */
void inicializar_mapa()
{
    for (int i = 0; i < ALTURA; i++)
    {
        for (int j = 0; j < LARGURA; j++)
        {
            mapa[i][j] = '.';
        }
    }

    // Posiciona o jogador (centro)
    mapa[jogador_x][jogador_y] = 'J';

    // Posiciona zumbis em posições aleatórias, evitando colocar muito perto do jogador
    for (int i = 0; i < NUM_ZUMBIS; i++)
    {
        int x, y;
        do
        {
            x = rand() % ALTURA;
            y = rand() % LARGURA;
        } while (abs(jogador_x - x) <= 3 || abs(jogador_y - y) <= 3);
        zumbis[i].x = x;
        zumbis[i].y = y;
        mapa[zumbis[i].x][zumbis[i].y] = 'Z';
    }
}

/*
Desenha o mapa na tela
Como iteramos o mapa, precisamos proteger com sem_mapa para evitar que outra thread
o modifique enquanto lemos
*/
void desenhar_mapa()
{
    sem_wait(sem_mapa); // Entra na região crítica do mapa

    for (int i = 0; i < ALTURA; i++)
    {
        for (int j = 0; j < LARGURA; j++)
        {
            switch (mapa[i][j])
            {
            case 'J':
                printf("😀"); // Jogador
                break;
            case 'Z':
                printf("🧟"); // Zumbi
                break;
            case 'P':
                printf("⭐"); // Power up
                break;
            case '.':
                printf("🌱"); // Vazio
                break;
            default:
                printf("%c", mapa[i][j]);
            }
        }
        printf("\n");
    }

    sem_post(sem_mapa); // Sai da região crítica do mapa

    /*
    Pontuação e power up:
    Pontos é segura por ser atômica
    Podemos ler "invencível" sem semáforo pois suas mudanças são feitas por
    uma thread e sincronizdas pelo sem_power (lemos esporadicamente)
    */
    printf("\nPontuação: %d\n", atomic_load(&pontos));
    sem_wait(sem_state);
    int inv_local = invencivel;
    sem_post(sem_state);
    if (inv_local)
    {
        printf("⭐ Invencível temporariamente!\n");
    }
}

/* FUNÇÕES DO JOGADOR */
/*
Tenta mover o jogador na direção especificada
Todo acesso/alteração no mapa é feito dentro da região crítica sem_mapa
Também trata coleta de power up e colisão com zumbis
*/
void mover_jogador(char direcao)
{
    int novo_x = jogador_x, novo_y = jogador_y;

    if (direcao == 'w' && novo_x > 0)
    {
        novo_x--;
    }

    if (direcao == 's' && novo_x < ALTURA - 1)
    {
        novo_x++;
    }

    if (direcao == 'a' && novo_y > 0)
    {
        novo_y--;
    }

    if (direcao == 'd' && novo_y < LARGURA - 1)
    {
        novo_y++;
    }

    char conteudo;
    sem_wait(sem_mapa); // Protege a leitura do mapa
    conteudo = mapa[novo_x][novo_y];
    sem_post(sem_mapa); // Libera o mapa

    // Se a posição contém power up, coleta
    if (conteudo == 'P')
    {
        sem_wait(sem_spawn);  // Atualiza estado do spawn (com exclusão)
        power_ativo_mapa = 0; // Retira indicação de power ativo no estado
        sem_post(sem_spawn);

        // Tenta ativar o efeito (se possível)
        if (sem_trywait(sem_power) == 0)
        {
            // Cria uma thread que aplica o efeito por tempo limitado
            pthread_t t;
            pthread_create(&t, NULL, thread_power_timer, NULL);
            pthread_detach(t);
        }
    }

    int invencivel_local;
    sem_wait(sem_state);
    invencivel_local = invencivel;
    sem_post(sem_state);

    // Se a posição tem zumbi e não estamos invencíveis -> game over
    if (conteudo == 'Z' && !invencivel_local)
    {
        sem_wait(sem_state);
        game_over = 1; // Alteração do estado global protegida por sem_state
        sem_post(sem_state);
    }

    sem_wait(sem_mapa); // Protege a escrita ao mapa
    // Limpa posição antiga e desenha jogador na nova
    mapa[jogador_x][jogador_y] = '.';
    jogador_x = novo_x;
    jogador_y = novo_y;
    mapa[jogador_x][jogador_y] = 'J';
    sem_post(sem_mapa); // libera o mapa
}

/* FUNÇÕES DO ZUMBI */
Zumbi mover_zumbi(Zumbi z)
{
    int dx, dy;
    sem_wait(sem_state); // Protege leitura do jogador
    dx = jogador_x - z.x;
    dy = jogador_y - z.y;
    sem_post(sem_state);

    int adx = abs(dx);
    int ady = abs(dy);

    int dist = adx + ady;
    int p_explore = 0;
    if (dist >= 10)
        p_explore = 60;
    else if (dist >= 4)
        p_explore = 30;
    else
        p_explore = 10;

    int nx = z.x, ny = z.y;

    if ((rand() % 100) < p_explore)
    {
        // Movimento aleatório
        int dir = rand() % 4;
        if (dir == 0 && nx > 0)
            nx--;
        else if (dir == 1 && nx < ALTURA - 1)
            nx++;
        else if (dir == 2 && ny > 0)
            ny--;
        else if (dir == 3 && ny < LARGURA - 1)
            ny++;
    }
    else
    {
        // Persege o jogador
        if (adx >= ady)
        {
            nx += (dx > 0) ? 1 : -1;
        }
        else
        {
            ny += (dy > 0) ? 1 : -1;
        }
    }

    // Limites
    if (nx < 0)
        nx = 0;
    if (nx >= ALTURA)
        nx = ALTURA - 1;
    if (ny < 0)
        ny = 0;
    if (ny >= LARGURA)
        ny = LARGURA - 1;

    Zumbi novo = {nx, ny};
    return novo;
}

/*
Calcula movimento aleatório e atualiza o mapa
Verifica game_over com sem_state para terminar direito
Checae colisão com jogador também é feita com sem_state
Atualização do mapa feita com sem_mapa
*/
void *thread_zumbi(void *arg)
{
    Zumbi *z = arg;

    while (1)
    {
        // Verifica término do jogo
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        // Escolhe direção aleatória
        Zumbi aux = mover_zumbi(*z);
        int nx = aux.x, ny = aux.y;

        // Checa colisão dentro de região crítica de estado
        sem_wait(sem_state);
        if (nx == jogador_x && ny == jogador_y && !invencivel)
        {
            game_over = 1;
        }
        sem_post(sem_state);

        // Atualiza mapa (movimenta o zumbi)
        sem_wait(sem_mapa);
        mapa[z->x][z->y] = '.';
        z->x = nx;
        z->y = ny;
        mapa[z->x][z->y] = 'Z';
        sem_post(sem_mapa);

        usleep(VEL_ZUMBI); // Pausa para controlar velocidade do zumbi
    }
    return NULL;
}

/* Thread de pontuação */
/*
Thread que roda em paralelo e incrementa a pontuação a cada 1 segundo
Verifica game_over via sem_state antes de incrementar
A variável pontos é atômica, permitindo incrementos sem bloqueio
*/
void *thread_pontuacao(void *arg)
{
    (void)arg;
    while (1)
    {
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        sleep(1);                      // 1 segundo
        atomic_fetch_add(&pontos, 10); // Aumenta 10 pontos
    }
    return NULL;
}

/* FUNÇÕES DO POWER UP */
/*
Cria um power up no mapa se não existir nenhum ativo e se o jogador não estiver invencível
sem_spawn protege o estado do spawn (power_ativo_mapa, power_x/y)
sem_mapa protege a escrita no mapa
*/
void spawn_powerup()
{
    int inv_local;
    sem_wait(sem_state);
    inv_local = invencivel;
    sem_post(sem_state);

    sem_wait(sem_spawn); // Protege o estado do power-up
    if (!power_ativo_mapa && !inv_local)
    {
        // Escolhe uma posição do mapa livre: por segurança verificamos sob sem_mapa antes
        int x, y;

        while (1)
        {
            x = rand() % ALTURA;
            y = rand() % LARGURA;

            sem_wait(sem_mapa); // Checagem da posição livre protegida por sem_mapa
            if (mapa[x][y] == '.')
            {
                mapa[x][y] = 'P';
                sem_post(sem_mapa);
                break;
            }
            sem_post(sem_mapa);
        }

        // Atualiza estado do spawn (região crítica)
        power_x = x;
        power_y = y;
        power_ativo_mapa = 1;
    }

    sem_post(sem_spawn); // Libera o semáforo de spawn
}

/*
Thread que gera power ups de forma assíncrona
ROda paralelamente e usa spawn_powerup() para inserir itens no mapa
*/
void *thread_spawn_powerups(void *arg)
{
    (void)arg;
    while (1)
    {
        // Verifica se o jogo acabou (estado protegido)
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        sleep(1 + rand() % 3); // Tempo aleatório entre spawns

        spawn_powerup(); // Faz spawn (região crítica)
    }
    return NULL;
}

/*
Thread que controla a duração do efeito do power up
Quando o jogador ativa o power up, essa thread é criada para gerir o timeout
sem_power é usado para evitar que dois efeitos sejam ativados ao mesmo tempo
*/
void *thread_power_timer(void *arg)
{
    (void)arg;

    sem_wait(sem_state);
    invencivel = 1; // Ativa efeito
    sem_post(sem_state);

    sleep(3); // Duração do efeito (3 segundos)

    sem_wait(sem_state);
    invencivel = 0; // Desativa
    sem_post(sem_state);

    // Libera permissionamento para permitir novo power up (semáforo do efeito)
    sem_post(sem_power);
    return NULL;
}

int main()
{
    srand(time(NULL));

    // Remove semáforos antigos caso o programa tenha terminado de forma estranha
    sem_unlink("/sem_mapa");
    sem_unlink("/sem_spawn");
    sem_unlink("/sem_power");
    sem_unlink("/sem_state");

    // Cria semáforos nomeados com valor inicial 1 (comportamento de mutex binário)
    sem_mapa = sem_open("/sem_mapa", O_CREAT, 0644, 1);
    sem_spawn = sem_open("/sem_spawn", O_CREAT, 0644, 1);
    sem_power = sem_open("/sem_power", O_CREAT, 0644, 1);
    sem_state = sem_open("/sem_state", O_CREAT, 0644, 1);

    // Inicializa mapa (não precisa de semáforo pois nenhuma thread existe ainda)
    inicializar_mapa();

    // Cria threads dos zumbis (cada uma roda thread_zumbi)
    pthread_t th_z[NUM_ZUMBIS];
    for (int i = 0; i < NUM_ZUMBIS; i++)
    {
        pthread_create(&th_z[i], NULL, thread_zumbi, &zumbis[i]);
    }

    // Thread responsável por spawnar power ups de forma independente
    pthread_t th_spawn;
    pthread_create(&th_spawn, NULL, thread_spawn_powerups, NULL);

    // Thread de pontuação que incrementa a cada segundo
    pthread_t th_pontos;
    pthread_create(&th_pontos, NULL, thread_pontuacao, NULL);

    // Loop principal
    while (1)
    {
        // Verifica se o jogo acabou (protegido)
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        limpar_tela();
        desenhar_mapa();

        if (kbhit())
        {
            mover_jogador(getchar());
        }

        usleep(100000); // Controla taxa de atualização do loop principal
    }

    // Fim do jogo: exibe placar e espera threads terminarem
    limpar_tela();
    printf("Você foi pego pelos zumbis!\n");
    printf("Pontuação final: %d\n", atomic_load(&pontos));

    for (int i = 0; i < NUM_ZUMBIS; i++)
    {
        pthread_join(th_z[i], NULL);
    }

    pthread_join(th_spawn, NULL);
    pthread_join(th_pontos, NULL);

    // Fecha e remove semáforos nomeados do sistema
    sem_close(sem_mapa);
    sem_unlink("/sem_mapa");
    sem_close(sem_spawn);
    sem_unlink("/sem_spawn");
    sem_close(sem_power);
    sem_unlink("/sem_power");
    sem_close(sem_state);
    sem_unlink("/sem_state");

    return 0;
}