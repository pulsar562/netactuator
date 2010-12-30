/*
 * This file is part of netactuator.
 *
 * netactuator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * netactuator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with netactuator.  If not, see <http://www.gnu.org/licenses/gpl.html>
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <rrd.h>

#include "defs.h"
#include "proto.h"
#include "mysql_driver.c"
#include "eventos.c"
#include "timing.c"
#include "datastruct.c"
#include "netmask.c"
#include "wrapper.c"
#include "topusers.c"
#include "file.c"
#include "blacklist.c"
#include "action.c"
#include "pattern.c"
#include "graph.c"
#include "limit.c"
#include "config.c"


// Mostra o modo de uso do aplicativo
int usage (tipostring programa)
{
    printf("\n");
    printf("Modo de uso: %s -c <config_file> [-v]", programa);
    printf("\n\n");
    printf("\n\n");

    return 1;
}


// Para o daemonize
int fork2 (void)
{
    pid_t pid;
    if (!(pid = fork()))
    {
        if (!fork())
            return(0);
        exit(0);
    }

    waitpid(pid, NULL, 0);

    return(1);
}


int main (int argc, char *argv[])
{
    int i=0;            // contador padr�o
    int erro=0;         // flag que indica se foi executado da forma correta na linha de comando
    char *in_opt=NULL;  // input file name
    FILE *in_handler;   // pointer to input file
    char ch;            // usado pelo getpot in the switch/case pair
    time_t data_hora_inicial;
    time_t data_hora_atual;
    struct tm *sdata;   // a ser usado no banco de dados
    tipostring comando; // string usada para efetuar comandos no sistema
    pthread_t thread;


    // Captura a data atual - a ser usado no DB - iteracao inicial (threads)
    time(&data_hora_inicial);
    sdata = localtime(&data_hora_inicial);
    sprintf(data, "%d-%d-%d", sdata->tm_year+1900, sdata->tm_mon+1, sdata->tm_mday);
    sprintf(hora, "%d:%d:%d", sdata->tm_hour, sdata->tm_min, sdata->tm_sec);
    sprintf(week_day, "%d", sdata->tm_wday);

    // Aponta para o local padr�o do arquivo de configura��o
    in_opt = "/usr/local/etc/netactuator/netactuator.conf";

    // Arquivo de input e output
    while ((ch = getopt(argc, argv, "c:v?h")) != -1)
        switch (ch)
        {
            case 'c':
                // Define o input_file
                free(in_opt);
                in_opt = optarg;
                break;

            case 'v':
                // Mostra a vers�o
                printf("\n");
                printf("netactuator version is: 1.0\n");
                // N�o tem break pois leva ao usage

            case '?':
            case 'h':
            default:
                erro = usage(argv[0]);
                exit(0);
        } // switch

    if (in_opt == NULL)
        erro = usage(argv[0]);

    // Se n�o houve erro, continua com a execu��o
    if (!erro)
    {
        // Coloca o processo em modo background / detach from console
        if (fork2())
            exit(0);

        // Abre arquivo de origem
//        if (in_handler = rfopen(in_opt, "rt")) // fechado dentro da thread, em config.c - carregar_configuracoes()
//        {
//            pthread_create(&thread, NULL, (void *) &carregar_configuracoes, (FILE *) in_handler);

            carregar_configuracoes(in_opt);

            // Aguarda at� a thread carregar todas as configs
            printf("Inicializando o netactuator...\n");
            while (flag_loading_config) // enquanto carregando configura��es, aguarda
            {
                printf("Aguardando as configs..\n");
                usleep(DELAY_MAIN);
            }

            // Inicia as outras threads
            pthread_create(&thread, NULL, (void *) &update_pattern_def, NULL);
            pthread_create(&thread, NULL, (void *) &on_the_fly, NULL);
            pthread_create(&thread, NULL, (void *) &graphing, NULL);

            mostrar_configuracoes();



/*
Passos:

1 - Coleta dados

2 - Agrupa dados

3 - Checa limites e armazena no banco (usando flags de clean e dirty) e gera gr�ficos
3.1 - Gera Gr�ficos

4 - Toma acao (block, mail)

5 - Fazer flutuacao de bloqueios

6 - Fazer atualizacao de padroes comportamentais

9 - Top Users

10 - Liberar memoria

// Verificar o numero da ocorrencia
// Caso maior que o max definido, adicionar em blacklist
*/



            // 1 - COLETA
            // 1 - SENSOR

            // Certifica-se de que nenhuma inst�ncia anterior do pmacctd esteja sendo executada
            system("kill -9 $(cat /var/run/pmacctd.pid*) > /dev/null 2>&1");
            system("killall pmacctd > /dev/null 2>&1");
            sleep(3);

            // Captura a data inicial - primeira
            time(&data_hora_inicial);

            // Executa o pmacctd para coleta de dados
            for(i=0; i<num_interfaces; i++)
            {
//printf("Lan�ando pmacctd %s\n", interfaces[i]);
                pmacctd_wrapper(interfaces[i]);
//fflush(stdout);
            }



            // La�o de monitora��o da rede
            while (1)
            {
                // Captura a data atual
                time(&data_hora_atual);

                // Se o tempo de captura chegou no seu limite e se n�o est� relendo as configs
                if (data_hora_atual >= (data_hora_inicial + flow_capture_time_sec) && (flag_loading_config == 0))
                {
                    // Captura a data inicial - atualiza no momento da coleta real e counter zeroing
                    time(&data_hora_inicial);
                    // Data a ser utilizada no RRD update, para n�o haver problema na demora entre os updates
                    time(&data_hora_global);
//printf("Nova coleta de stats...\n");
                    // Reset na raiz
                    raiz = NULL;
                    // Reset no contador de linhas dos arquivos gerados pelo pmacctd
                    count_log = 0;
                    // Reset nas listas
                    top_users_frames_recv = NULL;
                    top_users_frames_sent = NULL;
                    top_users_bytes_recv = NULL;
                    top_users_bytes_sent = NULL;
                    top_users_convs_as_source = NULL;
                    top_users_convs_as_destin = NULL;


                    inicial();
                    // 2 - COLLECTOR - AGRUPAMENTO
                    // Processa a coleta e gera estat�sticas
                    for(i=0; i<num_interfaces; i++)
                        processar_coleta_gerada(interfaces[i]);
                    final("netactuator LOG PARSE - BIN SEARCH TO MEMORY");


                    // Captura a data atual - a ser usado no DB - ajuste (quebra)
                    sdata = localtime(&data_hora_inicial);
                    sprintf(data, "%d-%d-%d", sdata->tm_year+1900, sdata->tm_mon+1, sdata->tm_mday);
                    sprintf(hora, "%d:%d:%d", sdata->tm_hour, sdata->tm_min, sdata->tm_sec);
                    sprintf(week_day, "%d", sdata->tm_wday);


                    inicial();
                    // 3 - CHECAGEM DE LIMITES e ARMAZENAMENTO NO BANCO com flags CLEAN e DIRTY
                    // 3.1 - GERA GR�FICOS
                    // 4 - TOMA A��O
                    // Checa se algum host ultrapassou os limites estabelecidos no arquivo de configura��o
                    // Armazena os dados no banco usando flags de limpo e sujo
                    // Toma as a��es necess�rias

                    // Cria o diret�rio dos graphs - criar logo antes de tentar usar, caso tenho sido deletado durante a
                    // execu��o reduz as chances de n�o se encontrar o diret�rio
                    sprintf(comando, "mkdir -p %s/graph", base_www);
                    system(comando);

                    // Conectar ao DB
                    if ((mysql_conn_global = conectar()))
                    {
                        // Dentro de checar_limites fica a gera��o dos gr�ficos pq o checar_limites jah varre a �rvore
                        // toda, e captura, para cada host, a sua baseline.
                        flag_limit_update = 1; // bloqueia gera��o de gr�ficos para evitar conflitos de acesso aos rrd
                        checar_limites(raiz);
                        desconectar(mysql_conn_global);
                        flag_limit_update = 0; // libera gera��o de gr�ficos
                    } // if conectar
                    final("LIMIT CHECK");


                    inicial();
                    // 9 - CRIA��O DE LISTAS DE TOP USERS
                    // Cria��o das listas de Top Users (vazias por enquanto, apenas espaco alocado)
                    top_users_frames_recv = criar_lista(top_users);
                    top_users_frames_sent = criar_lista(top_users);
                    top_users_bytes_recv = criar_lista(top_users);
                    top_users_bytes_sent = criar_lista(top_users);
                    top_users_convs_as_source = criar_lista(top_users);
                    top_users_convs_as_destin = criar_lista(top_users);

                    gerar_top_users(raiz);
                    final("GERAR TOP USERS");


//                    mostrar_top_users("convs_as_source");
//                    mostrar_top_users("convs_as_destin");
//                    mostrar_top_users("frames_recv");
//                    mostrar_top_users("frames_sent");
//                    mostrar_top_users("bytes_recv");
//                    mostrar_top_users("bytes_sent");

//inicial();
//                    printf("Registros logs (linhas com dois IPs): %ld\n", count_log);
//                    printf("Registros arvore (um IP): %ld\n", contar_registros_arvore(raiz, 0));
//final("CONTAR REGISTROS");


                    inicial();
                    gerar_top_users_db();
                    final("GERAR TOP USERS DB");

                    // 10 - LIBERA��O DA MEM�RIA
                    destruir_arvore(raiz);
                    destruir_lista(top_users_frames_recv);
                    destruir_lista(top_users_frames_sent);
                    destruir_lista(top_users_bytes_recv);
                    destruir_lista(top_users_bytes_sent);
                    destruir_lista(top_users_convs_as_source);
                    destruir_lista(top_users_convs_as_destin);
                } // if data_hora do collector (flow_cap_time)

                // Para imprimir as sa�das na tela quando h� redir no Shell
                fflush(stdout);

                usleep(DELAY_MAIN);    // delay em cada itera��o do la�o principal
            } // while (1)
//        } // if in_handler
    } // if !erro

    return 0;
}
