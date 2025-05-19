#include <mpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
/* wątki */
#include <pthread.h>
/* sem_init sem_destroy sem_post sem_wait */
//#include <semaphore.h>
/* flagi dla open */
//#include <fcntl.h>
/* boolean */
#define TRUE 1
#define FALSE 0

/* typy wiadomości */
//#define FINISH 1
//#define APP_MSG 2
#define REQ_A 1
#define REQ_G 2
#define ACK_A 3
#define REQ_SLOT 4
#define RELEASE_SLOT 5

#define MAX_SLOTS 10
#define MAX_ARTISTS 10
#define MAX_ENGINEERS 10

#define ROLE_A 0 
#define ROLE_G 1 

/* inicjalizacja zegara Lamporta */
int lamport_clock = 0;

char passive = FALSE;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

/* komunikatory grupowe */

MPI_Comm artist_comm = MPI_COMM_NULL;
MPI_Comm engineer_comm = MPI_COMM_NULL;

/* typy wiadomości */

MPI_Datatype MPI_MESSAGE_T;
MPI_Datatype MPI_SLOT_REQUEST_T;

typedef struct {
    int type;
    int sender_id;
    int clock;
} message;

typedef struct {
    int type;
    int sender_id;
    int clock;
    int g_pair;
    int num_slots;
} slot_request;

int role;
int pending_req[count_engineers];
int priority[count_engineers];

/* Zwiększa zegar Lamporta o 1 (przed wysłaniem wiadomości) */
void increment_lamport() {
    pthread_mutex_lock(&mut);
    lamport_clock++;
    pthread_mutex_unlock(&mut);
}

/* Aktualizuje zegar Lamporta po otrzymaniu wiadomości */
void update_lamport(int received_clock) {
    pthread_mutex_lock(&mut);
    if (lamport_clock < received_clock) {
        lamport_clock = received_clock;
    }
    lamport_clock++;
    pthread_mutex_unlock(&mut);
}

/* Zwraca aktualną wartość zegara */
int get_lamport() {
    pthread_mutex_lock(&mut);
    int val = lamport_clock;
    pthread_mutex_unlock(&mut);
    return val;
}

/* MPI_Send wraper obługujący zegar lamporta */
int lamport_send(void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {
    // Ustaw zegar w strukturze wiadomości — zależnie od typu
    if (datatype == MPI_MESSAGE_T) {
        ((message*)buf)->clock = get_lamport();
    } else if (datatype == MPI_SLOT_REQUEST_T) {
        ((slot_request*)buf)->clock = get_lamport();
    }

    return MPI_Send(buf, count, datatype, dest, tag, comm);
}

void create_message_types() {
    const int nitems = 3;
    int blocklengths[3] = {1, 1, 1};
    MPI_Datatype typy[3] = {MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets[3];

    offsets[0] = offsetof(message, type);
    offsets[1] = offsetof(message, sender_id);
    offsets[2] = offsetof(message, clock);

    MPI_Type_create_struct(nitems, blocklengths, offsets, typy, &MPI_MESSAGE_T);
    MPI_Type_commit(&MPI_MESSAGE_T);

    const int nitems2 = 5;
    int blocklengths2[5] = {1, 1, 1, 1, 1};
    MPI_Datatype typy2[5] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets2[5];

    offsets2[0] = offsetof(slot_request, type);
    offsets2[1] = offsetof(slot_request, sender_id);
    offsets2[2] = offsetof(slot_request, clock);
    offsets2[3] = offsetof(slot_request, g_pair);
    offsets2[4] = offsetof(slot_request, num_slots);

    MPI_Type_create_struct(nitems2, blocklengths2, offsets2, typy2, &MPI_SLOT_REQUEST_T);
    MPI_Type_commit(&MPI_SLOT_REQUEST_T);
}

void create_role_comms(MPI_Comm world_comm, int rank, int size) {
    int artist_ranks[MAX_ARTISTS];
    int engineer_ranks[MAX_ENGINEERS];
    int artist_count = 0, engineer_count = 0;

    for (int i = 0; i < size; i++) {
        if (i < MAX_ARTISTS)
            artist_ranks[artist_count++] = i;
        else
            engineer_ranks[engineer_count++] = i;
    }

    MPI_Group world_group;
    MPI_Comm_group(world_comm, &world_group);

    MPI_Group artist_group, engineer_group;
    MPI_Group_incl(world_group, artist_count, artist_ranks, &artist_group);
    MPI_Group_incl(world_group, engineer_count, engineer_ranks, &engineer_group);

    if (rank < MAX_ARTISTS)
        MPI_Comm_create(world_comm, artist_group, &artist_comm);
    else
        MPI_Comm_create(world_comm, engineer_group, &engineer_comm);
}

void send_message_to_artists(message *msg, int tag) {
    increment_lamport();
    for (int i = 0; i < MAX_ARTISTS; i++) {
        if (i != msg->sender_id) { // nie wysyłaj do siebie
            lamport_send(msg, 1, MPI_MESSAGE_T, i, tag, MPI_COMM_WORLD);
        }
    }
}

void send_message_to_engineers(message *msg, int tag) {
    increment_lamport();
    for (int i = MAX_ARTISTS; i < MAX_ARTISTS + MAX_ENGINEERS; i++) {
        if (i != msg->sender_id) {
            lamport_send(msg, 1, MPI_MESSAGE_T, i, tag, MPI_COMM_WORLD);
        }
    }
}

void send_message_to_process(message *msg, int dest, int tag) {
    increment_lamport();
    lamport_send(msg, 1, MPI_MESSAGE_T, dest, tag, MPI_COMM_WORLD);
}

void send_message_to_all(void *msg, int count, MPI_Datatype datatype, int tag, int self_rank, int comm_size) {
    increment_lamport();
    for (int dest = 0; dest < comm_size; dest++) {
        if (dest != self_rank) {
            lamport_send(msg, count, datatype, dest, tag, MPI_COMM_WORLD);
        }
    }
}


void *startFunc(void *ptr)
{
    int thread_role = *((int *)ptr);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (thread_role == ROLE_A) { // what artist does
        printf("Thread A (Artist) started on rank %d\n", rank);
    }
    else if (thread_role == ROLE_G) { //what engineer does
        printf("Thread G (Engineer) started on rank %d\n", rank);
    }

    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    printf("poczatek\n");
    int provided;

    MPI_Init_thread(&argc, &argv,MPI_THREAD_MULTIPLE, &provided);

    printf("THREAD SUPPORT: %d\n", provided);
    switch (provided) {
        case MPI_THREAD_SINGLE: 
            printf("Brak wsparcia dla wątków, kończę\n");
            /* Nie ma co, trzeba wychodzić */
	    fprintf(stderr, "Brak wystarczającego wsparcia dla wątków - wychodzę!\n");
	    MPI_Finalize();
	    exit(-1);
	    break;
        case MPI_THREAD_FUNNELED: 
            printf("tylko te wątki, ktore wykonaly mpi_init_thread mogą wykonać wołania do biblioteki mpi\n");
	    break;
        case MPI_THREAD_SERIALIZED: 
            /* Potrzebne zamki wokół wywołań biblioteki MPI */
            printf("tylko jeden watek naraz może wykonać wołania do biblioteki MPI\n");
	    break;
        case MPI_THREAD_MULTIPLE: printf("Pełne wsparcie dla wątków\n");
	    break;
        default: printf("Nikt nic nie wie\n");
    }

    create_message_types();

    pthread_t threadA, threadG;
    int roleA = ROLE_A;
    int roleG = ROLE_G;

    pthread_create(&threadA, NULL, startFunc, &roleA);
    pthread_create(&threadG, NULL, startFunc, &roleG);

    /* Zamykanie muteksa */
  //pthread_mutex_lock(&mut);
    /* Otwieranie muteksa */
  //pthread_mutex_unlock(&mut);


    int size,   ;
    char end = FALSE;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    MPI_Status status;
    int data;     
    message pakiet;

    if   {  
        /* Usuń kod, jest tylko po to, by przypomnieć działanie MPI_Send
           oraz, by program przykładowy się zakończył */
        int i;
        for (i=1;i<size;i++) {
	    MPI_Send(&pakiet, 1, MPI_PAKIET_T, i, FINISH, MPI_COMM_WORLD);
            printf("Poszło do %d\n", i);
        }
        end = TRUE;
    } 

    srand(rank);

    /* Obrazuje pętlę odbierającą pakiety o różnych typach */
    while ( !end ) {
    MPI_Recv( &pakiet, 1, MPI_PAKIET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

       switch (status.MPI_TAG) {
	    case FINISH: end = TRUE; break;
            case APP_MSG: 
                passive = FALSE;
                break;
            /* więcej case */
            default: 
                break;
       }

    }

    pthread_mutex_destroy( &mut);

    /* Czekamy, aż wątek potomny się zakończy */
    pthread_join(threadA, NULL);
    pthread_join(threadG, NULL);

    MPI_Type_free(&MPI_PAKIET_T);
    MPI_Finalize();
}
