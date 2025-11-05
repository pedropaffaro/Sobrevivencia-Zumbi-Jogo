# Jogo de Sobrevivência - Zumbis

## 1. Introdução

Este documento apresenta a especificação técnica e o manual do jogo de sobrevivência desenvolvido como trabalho prático para a disciplina de Sistemas Operacionais. O projeto tem como principal objetivo aplicar e demonstrar o uso de **threads**, **semáforos** e **operações atômicas**, conceitos fundamentais em concorrência e sincronização de processos em sistemas computacionais.

O jogo consiste em um mapa bidimensional no qual um jogador deve se movimentar e evitar zumbis que circulam pelo ambiente. O usuário controla o personagem em tempo real, enquanto múltiplas threads gerenciam o comportamento dinâmico dos elementos do jogo.

## 2. Objetivo do Jogo

O jogador deve **sobreviver o maior tempo possível**. A cada segundo sobrevivido, é atribuido **10 pontos** ao jogador. O jogo termina quando o personagem é alcançado por um zumbi, exceto quando o jogador se encontra sob efeito de invencibilidade concedido por um power-up.

## 3. Controles do Usuário

| Tecla | Função              |
| ----- | ------------------- |
| W     | Mover para cima     |
| A     | Mover para esquerda |
| S     | Mover para baixo    |
| D     | Mover para direita  |

## 4. Elementos do Jogo

| Símbolo | Descrição                             |
| ------- | ------------------------------------- |
| 😀      | Jogador                               |
| 🧟      | Zumbi                                 |
| ⭐       | Power-up (invencibilidade temporária) |
| 🌱      | Espaço livre                          |

## 5. Arquitetura do Sistema

### 5.1 Estrutura de Concorrência

O sistema utiliza **múltiplas threads** executando de forma concorrente:

| Thread                       | Responsabilidade                                      |
| ---------------------------- | ----------------------------------------------------- |
| Thread principal             | Captura entrada do usuário e atualiza o estado visual |
| 6 Threads de zumbis          | Movimentação independente dos zumbis pelo mapa      |
| Thread de spawn de power-ups | Geração de power-ups de forma aleatória               |
| Thread do power-up           | Controla a duração da invencibilidade                 |
| Thread de pontuação          | Incrementa a pontuação a cada segundo                 |

Esse modelo ilustra o uso de **multithreading cooperativo**, onde cada thread realiza uma função específica, permitindo simulação contínua e responsiva.

### 5.2 Mecanismos de Sincronização

A sincronização entre threads é realizada por meio de **semáforos nomeados**, garantindo exclusão mútua e prevenindo condições de corrida.

| Semáforo    | Função                                                 |
| ----------- | ------------------------------------------------------ |
| `sem_mapa`  | Controle de acesso à matriz do mapa (região crítica)   |
| `sem_spawn` | Protege o estado de existência de um power-up no mapa  |
| `sem_power` | Garante que apenas um power-up seja ativado por vez    |
| `sem_state` | Protege variáveis de estado do jogo (ex.: `game_over`) |

### 5.3 Operações Atômicas

A variável de pontuação utiliza `atomic_int`, garantindo atualização correta em ambiente concorrente sem necessidade de bloqueios explícitos.

Esse recurso evita **race conditions** em operações simples, otimizando a eficiência sem perder segurança.

### 5.4 Regiões Críticas

Os seguintes trechos foram protegidos por semáforos:

* Atualização da posição do jogador
* Movimentação dos zumbis
* Inserção e coleta de power-ups
* Verificação de condição de término do jogo

## 6. Funcionamento do Sistema

### 6.1 Loop do Jogo

O ciclo principal realiza:

1. Leitura não bloqueante do teclado
2. Atualização da posição do jogador
3. Renderização do mapa
4. Verificação de `game_over`

### 6.2 Encerramento

Quando um zumbi alcança o jogador:

* `game_over` é definido como `1`
* Todas as threads verificam o estado e se encerram
* A pontuação final é exibida ao usuário

