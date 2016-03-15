#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include <sys/wait.h>
#include <signal.h>
#include <infiniband/verbs.h>

#define CHECK(cond) {\
   if((cond)==0) {\
      puts("Error");\
      exit(-1);\
   }\
}

#define CHECK_MSG(cond, msg) {\
   if((cond)==0) {\
      puts(msg);\
      exit(-1);\
   }\
}

#define SIZE 10000
#define MYPORT 18515
time_t timer;
int sockfd, new_fd; // listen on sock_fd, new connection on new_fd
struct sockaddr_in my_addr; // my address information
struct sockaddr_in their_addr; // connector.s address information
int sin_size;
int yes=1;

struct context {
   struct ibv_context* context;
   struct ibv_pd* pd;
   struct ibv_mr* mr;
   struct ibv_qp* qp;
   struct ibv_cq* send_cq;
   struct ibv_cq* recv_cq;
   
   unsigned int local_rkey;  // rkey to be sent to client
   union ibv_gid gid;
   int qpn;
   int lid;
   
} s_ctx;

void exchange_bootstrap_data(void* virtual_address, int rkey)
{
    puts("exchange_bootstrap_data starting");
    // The server needs to send the rkey
    // and the virtual address
    char data_to_send[1000];
    memset(data_to_send, 0, 1000);

    sprintf(data_to_send, "%016Lx:%08x", (unsigned long long int)virtual_address, rkey);

    int retval = send(new_fd, data_to_send, strlen(data_to_send), 0);
    
    printf("sent data: %s\n", data_to_send);

    char recv_buffer[1000];
    memset(recv_buffer, 0, 1000);

    retval = recv(new_fd, recv_buffer, 1000, 0);
    CHECK(retval > 0);
    
    printf("received: %s\n", recv_buffer);
}

void handshake()
{
// lid, out_Reads, qpn, psn, rkey, vaddr, srqn
    struct ibv_port_attr attr;
    int retval = ibv_query_port(s_ctx.context,1,&attr);
    CHECK(retval == 0);

    s_ctx.lid = attr.lid;
    s_ctx.qpn = s_ctx.qp->qp_num;
    //s_ctx.psn = lrand48() & 0xffffff;
    s_ctx.local_rkey = s_ctx.mr->rkey;

    char* buffer = (char*) malloc(1000);

    ibv_query_gid(s_ctx.context, 1, 0, &s_ctx.gid);

//    char my_data[100];
//    sprintf(my_data, "

    // client needs to get the rkey and remote_address
    exchange_bootstrap_data(buffer, s_ctx.local_rkey);

    return;
}


int setup_rdma()
{
   struct ibv_device **dev_list;
   struct ibv_device *ibv_dev;

   int num_devices;
   dev_list = ibv_get_device_list(&num_devices);

   ibv_dev = dev_list[0];
   CHECK_MSG(ibv_dev != NULL, "Error getting device");

   s_ctx.context = ibv_open_device(ibv_dev);
   CHECK_MSG(s_ctx.context != NULL, "Error getting cntext");

   s_ctx.pd = ibv_alloc_pd(s_ctx.context);
   CHECK_MSG(s_ctx.pd != 0, "Error gettign pd");

   char* buf = (char*)malloc(SIZE);
   strcpy(buf, "AHOY!");
   CHECK_MSG(buf != 0, "Error getting buf");
   s_ctx.mr = ibv_reg_mr(s_ctx.pd, buf, SIZE, IBV_ACCESS_LOCAL_WRITE | 
                            IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
   CHECK_MSG(s_ctx.mr != NULL, "Error getting mr");
   
   // create channel
   struct ibv_comp_channel *event_channel;
   event_channel = ibv_create_comp_channel(s_ctx.context);
   
   s_ctx.send_cq = ibv_create_cq(s_ctx.context, 100, event_channel, 0, 0); 
   CHECK_MSG(s_ctx.send_cq != 0, "Error getting cq");
   s_ctx.recv_cq = ibv_create_cq(s_ctx.context, 100, event_channel, 0, 0); 
   CHECK_MSG(s_ctx.recv_cq != 0, "Error getting recv cq");

   struct ibv_qp_init_attr qp_attr;
   memset(&qp_attr, 0, sizeof(struct ibv_qp_init_attr));
   qp_attr.send_cq = s_ctx.send_cq;
   qp_attr.recv_cq = s_ctx.recv_cq;
   qp_attr.cap.max_send_wr  = 1;
   qp_attr.cap.max_recv_wr  = 1;
   qp_attr.cap.max_send_sge = 1;
   qp_attr.cap.max_recv_sge = 1;
   qp_attr.cap.max_inline_data = 0;
   qp_attr.qp_type = IBV_QPT_RC;

   s_ctx.qp = ibv_create_qp(s_ctx.pd, &qp_attr);
   CHECK_MSG(s_ctx.qp != NULL, "Error getting qp");

   puts("Moving QP to init");

   struct ibv_qp_attr attr;
   memset(&attr, 0, sizeof(attr));
    
   attr.qp_state        = IBV_QPS_INIT;
   attr.pkey_index      = 0;
   attr.port_num        = 1;
   attr.qp_access_flags = 0;

   int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
   int retval = ibv_modify_qp(s_ctx.qp, &attr, flags);
   CHECK_MSG(retval == 0, "Error modifying qp");

   puts("Handshaking");
   handshake();

   CHECK(ibv_req_notify_cq(s_ctx.send_cq, 0) == 0);
   CHECK(ibv_req_notify_cq(s_ctx.recv_cq, 0) == 0);

   void *ctx;
   CHECK(ibv_get_cq_event(event_channel, &s_ctx.recv_cq, &ctx) == 0);
   
   puts("Got event");
}

void wait_for_tcp_connection()
{
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }
    if (setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(int)) == -1)
    {
        perror("setsockopt");
        exit(1);
    }
    my_addr.sin_family = AF_INET; // host byte order
    my_addr.sin_port = htons(MYPORT); // short, network byte order
    my_addr.sin_addr.s_addr = INADDR_ANY; // automatically fill with my IP
    memset(&(my_addr.sin_zero), 0, 8); // zero the rest of the struct

    if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("bind");
        exit(1);
    }
    if (listen(sockfd, 10) == -1)
    {
        perror("listen");
        exit(1);
    }
    
    sin_size = sizeof(struct sockaddr_in);
    if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr,&sin_size)) == -1)
    {
	    //perror("accept");
	    exit(-1);
    }
    printf("Received request from Client: %s:%d\n",
		    inet_ntoa(their_addr.sin_addr),MYPORT);
}

int main(void)
{
    timer = time(NULL);

    wait_for_tcp_connection();
    setup_rdma();

    while(1);

    close(new_fd);
    exit(0);
}

