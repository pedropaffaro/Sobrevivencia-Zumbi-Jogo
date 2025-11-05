#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <stdatomic.h>

/* --- Constantes do jogo --- */
#define ALTURA 10
#define LARGURA 20
#define NUM_ZUMBIS 6

/* --- Estado compartilhado --- */
// mapa de caracteres representando o mundo do jogo.
// Cada célula: '.' (vazio), 'J' (jogador), 'Z' (zumbi), 'P' (power-up)
char mapa[ALTURA][LARGURA];

// posição do jogador (variáveis globais lidas por várias threads)
int jogador_x = ALTURA / 2;
int jogador_y = LARGURA / 2;

// flag de término do jogo (acessada por múltiplas threads)
// protegida por sem_state quando necessário
int game_over = 0;

/* --- Estrutura para zumbis --- */
typedef struct {
    int x, y; // posição do zumbi
} Zumbi;

Zumbi zumbis[NUM_ZUMBIS];

/* --- Semáforos (recursos separados) ---
   Uso de semáforos nomeados para demonstrar POSIX semáforos:
   - sem_mapa : exclusão mútua do array mapa[][]
   - sem_spawn: protege estado do power-up (power_ativo_mapa, power_x, power_y)
   - sem_power: controla ativação do efeito do power-up (impede stacking)
   - sem_state: protege estado do jogo (game_over) e leituras sensíveis
*/
sem_t *sem_mapa;     // protege a matriz do mapa
sem_t *sem_spawn;    // protege o estado de spawn de power-up
sem_t *sem_power;    // evita ativação simultânea do efeito do power-up
sem_t *sem_state;    // protege game_over e leituras de estado

/* --- Estado do power-up --- */
int power_x = -1, power_y = -1;   // posição do power-up no mapa
int power_ativo_mapa = 0;         // indica se há power-up no mapa (0/1)
int invencivel = 0;               // flag de efeito (1 enquanto invencível)

/* --- Pontuação --- */
// atomic_int é usado para garantir que incrementos/leitura sejam seguros
// sem a necessidade de semáforo apenas para a pontuação.
atomic_int pontos = 0;

/* ---------------- Funções auxiliares ---------------- */

/* limpa a tela (sequência ANSI) */
void limpar_tela()
{
    printf("\033[H\033[J");
}

/* kbhit: lê teclado sem bloqueio (não precisa dar ENTER)
   técnica: altera o modo do terminal para não canônico e não ecoa.
   Retorna 1 se há tecla pronta, 0 caso contrário.
*/
int kbhit(void)
{
    struct termios oldt, newt;
    int ch, oldf;
    tcgetattr(STDIN_FILENO, &oldt);             // guarda estado antigo
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);           // modo não-canônico, sem eco
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);    // aplica novo modo
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK); // stdin não bloqueante

    ch = getchar();                             // tenta ler

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);    // restaura terminal
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF)
    {
        ungetc(ch, stdin); // coloca o caractere de volta na stream
        return 1;
    }
    return 0;
}

/* ---------------- Funções para o mapa ---------------- */

/* inicializa o mapa (chamar antes de criar threads) */
void inicializar_mapa()
{
    for (int i = 0; i < ALTURA; i++)
        for (int j = 0; j < LARGURA; j++)
            mapa[i][j] = '.';

    // posiciona o jogador (centro)
    mapa[jogador_x][jogador_y] = 'J';

    // posiciona zumbis em posições aleatórias — evitando colocar sobre o jogador
    for (int i = 0; i < NUM_ZUMBIS; i++)
    {
        int x, y;
        do {
            x = rand() % ALTURA;
            y = rand() % LARGURA;
        } while (x == jogador_x && y == jogador_y);
        zumbis[i].x = x;
        zumbis[i].y = y;
        mapa[zumbis[i].x][zumbis[i].y] = 'Z';
    }
}

/* desenha o mapa na tela.
   Observação: como iteramos o mapa, precisamos proteger com sem_mapa
   para evitar que outra thread o modifique enquanto lemos.
*/
void desenhar_mapa()
{
    sem_wait(sem_mapa); // entra na região crítica do mapa

    for (int i = 0; i < ALTURA; i++)
    {
        for (int j = 0; j < LARGURA; j++)
        {
            switch (mapa[i][j])
            {
            case 'J':
                printf("😀"); // jogador
                break;
            case 'Z':
                printf("🧟"); // zumbi
                break;
            case 'P':
                printf("⭐"); // power-up
                break;
            case '.':
                printf("🌱"); // vazio
                break;
            default:
                printf("%c", mapa[i][j]);
            }
        }
        printf("\n");
    }

    sem_post(sem_mapa); // sai da região crítica do mapa

    /* HUD (mostra pontuação e estado do power-up)
       A leitura de `pontos` é segura por ser atômica.
       A leitura de `invencivel` pode ocorrer sem semáforo pois:
       - mudanças em `invencivel` são feitas por thread_power_timer e
         sincronizadas logicamente via sem_power; leitura esporádica é aceitável
       - se desejar rigor, proteger leitura com sem_state (opcional)
    */
    printf("\nPontuação: %d\n", atomic_load(&pontos));
    if (invencivel)
        printf("⭐ Invencível temporariamente! 💪\n");
}

/* ------------- Funções para o power-up ------------- */

/* verifica se posição é válida para spawn (vazio).
   IMPORTANTE: esta função acessa `mapa` sem lock — quem a chama deve garantir
   que a verificação/escrita sejam feitas com proteção adequada.
*/
int posicao_valida_spawn(int x, int y)
{
    return mapa[x][y] == '.';
}

/* spawn_powerup: cria um power-up no mapa se não existir nenhum ativo e se
   o jogador não estiver invencível. Duas responsabilidades separadas:
   - sem_spawn protege o estado do spawn (power_ativo_mapa, power_x/y)
   - sem_mapa protege a escrita efetiva no mapa
*/
void spawn_powerup()
{
    sem_wait(sem_spawn); // protege o estado do power-up

    if (!power_ativo_mapa && !invencivel)
    {
        int x, y;
        // escolhe uma célula livre: por segurança verificamos sob sem_mapa antes
        do
        {
            x = rand() % ALTURA;
            y = rand() % LARGURA;

            // checagem da célula livre protegida por sem_mapa
            sem_wait(sem_mapa);
            int valido = (mapa[x][y] == '.');
            sem_post(sem_mapa);
            if (valido) break;
        } while (1);

        // atualiza estado do spawn (região crítica do spawn)
        power_x = x;
        power_y = y;
        power_ativo_mapa = 1;

        // escreve no mapa: região crítica do mapa
        sem_wait(sem_mapa);
        mapa[x][y] = 'P';
        sem_post(sem_mapa);
    }

    sem_post(sem_spawn); // libera o semáforo de spawn
}

/* thread que gera power-ups de forma assíncrona
   Ela roda paralelamente e usa spawn_powerup() para inserir itens no mundo.
*/
void *thread_spawn_powerups(void *arg)
{
    (void)arg;
    while (1)
    {
        // verifica se o jogo acabou (estado protegido)
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        sleep(1 + rand() % 3); // tempo aleatório entre spawns

        spawn_powerup();       // faz spawn (regiões críticas corretamente protegidas)
    }
    return NULL;
}

/* thread que controla a duração do efeito do power-up.
   Quando o jogador ativa o power-up, essa thread é criada para gerir o timeout.
   sem_power é usado para evitar que dois efeitos sejam ativos simultaneamente.
*/
void *thread_power_timer(void *arg)
{
    (void)arg;
    invencivel = 1; // ativa efeito (não protegido por sem_state — leitura/escrita simples)
    sleep(3);       // duração do efeito (3 segundos)
    invencivel = 0; // desativa

    // libera permissionamento para permitir novo power-up (semáforo do efeito)
    sem_post(sem_power);
    return NULL;
}

/* ------------- Funções para o jogador ------------- */

/* mover_jogador: tenta mover o jogador na direção especificada.
   Todo acesso/alteração ao mapa é feito dentro da região crítica sem_mapa.
   Também trata coleta de power-up e colisão com zumbis.
*/
void mover_jogador(char direcao)
{
    int novo_x = jogador_x, novo_y = jogador_y;

    if (direcao == 'w' && novo_x > 0)
        novo_x--;
    if (direcao == 's' && novo_x < ALTURA - 1)
        novo_x++;
    if (direcao == 'a' && novo_y > 0)
        novo_y--;
    if (direcao == 'd' && novo_y < LARGURA - 1)
        novo_y++;

    sem_wait(sem_mapa); // protege leitura/escrita do mapa

    // se a célula de destino contém power-up, coleta:
    if (mapa[novo_x][novo_y] == 'P')
    {
        sem_wait(sem_spawn);          // atualiza estado do spawn com exclusão
        power_ativo_mapa = 0;         // retira indicação de power ativo no estado
        sem_post(sem_spawn);

        // tenta ativar o efeito (se possível)
        if (sem_trywait(sem_power) == 0)
        {
            // cria uma thread que aplica o efeito por tempo limitado
            pthread_t t;
            pthread_create(&t, NULL, thread_power_timer, NULL);
            // nota: não fazemos pthread_detach aqui por simplicidade; a thread terminará logo
        }
    }

    // se a célula tem zumbi e não estamos invencíveis -> game over
    if (mapa[novo_x][novo_y] == 'Z' && !invencivel)
    {
        sem_wait(sem_state);
        game_over = 1; // alteração do estado global protegida por sem_state
        sem_post(sem_state);
    }

    // atualiza mapa: limpa posição antiga e desenha jogador na nova
    mapa[jogador_x][jogador_y] = '.';
    jogador_x = novo_x;
    jogador_y = novo_y;
    mapa[jogador_x][jogador_y] = 'J';

    sem_post(sem_mapa); // libera o mapa
}

/* ------------- Funções para o zumbi ------------- */

/* thread de cada zumbi: calcula movimento aleatório e atualiza o mapa.
   Observações sobre sincronização:
   - verifica game_over sob sem_state para terminar corretamente
   - checagem de colisão com jogador também é feita sob sem_state
   - atualização do mapa feita sob sem_mapa
*/
void *thread_zumbi(void *arg)
{
    Zumbi *z = arg;

    while (1)
    {
        // verifica término do jogo
        sem_wait(sem_state);
        if (game_over)
        {
            sem_post(sem_state);
            break;
        }
        sem_post(sem_state);

        // escolhe direção aleatória
        int nx = z->x, ny = z->y;
        int dir = rand() % 4;

        if (dir == 0 && nx > 0)
            nx--;
        if (dir == 1 && nx < ALTURA - 1)
            nx++;
        if (dir == 2 && ny > 0)
            ny--;
        if (dir == 3 && ny < LARGURA - 1)
            ny++;

        // checa colisão dentro de região crítica de estado
        sem_wait(sem_state);
        if (nx == jogador_x && ny == jogador_y && !invencivel)
            game_over = 1;
        sem_post(sem_state);

        // atualiza mapa (movimenta o zumbi)
        sem_wait(sem_mapa);
        mapa[z->x][z->y] = '.';
        z->x = nx;
        z->y = ny;
        mapa[z->x][z->y] = 'Z';
        sem_post(sem_mapa);

        usleep(300000); // pausa para controlar velocidade do zumbi
    }
    return NULL;
}

/* ------------- Thread de pontuação ------------- */

/* thread que roda em paralelo e incrementa a pontuação a cada 1 segundo.
   Ela verifica game_over via sem_state antes de cada incremento.
   A variável pontos é atômica, permitindo incrementos sem bloqueio.
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

        sleep(1); // a cada segundo
        atomic_fetch_add(&pontos, 10); // aumenta 10 pontos de forma atômica
    }
    return NULL;
}

/* ---------------- Função Principal ---------------- */

int main()
{
    srand(time(NULL));

    // remove semáforos antigos caso o programa tenha terminado de forma anormal
    sem_unlink("/sem_mapa");
    sem_unlink("/sem_spawn");
    sem_unlink("/sem_power");
    sem_unlink("/sem_state");

    // cria semáforos nomeados com valor inicial 1 (comportamento de mutex binário)
    sem_mapa = sem_open("/sem_mapa", O_CREAT, 0644, 1);
    sem_spawn = sem_open("/sem_spawn", O_CREAT, 0644, 1);
    sem_power = sem_open("/sem_power", O_CREAT, 0644, 1);
    sem_state = sem_open("/sem_state", O_CREAT, 0644, 1);

    // inicializa mapa (não precisa de semáforo pois nenhuma thread existe ainda)
    inicializar_mapa();

    // cria threads dos zumbis (cada uma roda thread_zumbi)
    pthread_t th_z[NUM_ZUMBIS];
    for (int i = 0; i < NUM_ZUMBIS; i++)
        pthread_create(&th_z[i], NULL, thread_zumbi, &zumbis[i]);

    // thread responsável por spawnar power-ups de forma independente
    pthread_t th_spawn;
    pthread_create(&th_spawn, NULL, thread_spawn_powerups, NULL);

    // thread de pontuação que incrementa a cada segundo
    pthread_t th_pontos;
    pthread_create(&th_pontos, NULL, thread_pontuacao, NULL);

    // loop principal: input + render
    while (1)
    {
        // verifica se o jogo acabou (protegido)
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
            mover_jogador(getchar());

        usleep(100000); // controla taxa de atualização do loop principal
    }

    // fim do jogo: exibe placar e espera threads terminarem
    limpar_tela();
    printf("💀 Você foi pego pelos zumbis! 💀\n");
    printf("Pontuação final: %d\n", atomic_load(&pontos));

    for (int i = 0; i < NUM_ZUMBIS; i++)
        pthread_join(th_z[i], NULL);

    pthread_join(th_spawn, NULL);
    pthread_join(th_pontos, NULL);

    // fecha e remove semáforos nomeados do sistema
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
