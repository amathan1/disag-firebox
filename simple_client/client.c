#include <infiniband/verbs.h>
#include <stdio.h>




void
main()
{
	// struct ibv_device **device_list = malloc(sizeof(ibv_device));
	
	// Initialize the structures
	ibv_fork_init();
	
	int num_devices;
	struct ibv_device** device_list;
	device_list = ibv_get_device_list(&num_devices);
	
	printf("Total Number of Devices : %d\n", num_devices);
	printf("Val: %p\n", device_list);

}
