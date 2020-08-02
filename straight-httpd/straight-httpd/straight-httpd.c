/*
  straight-httpd.c
  Author: Straight Coder<simpleisrobust@gmail.com>
  Date: April 12, 2020
*/

#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <time.h>

#include "../../lwip-port/win32/arch/port.h"

#pragma warning(disable:4996)

#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")

#include "pcap.h"
#pragma comment(lib, "../../pcap/lib/Packet.lib")
#pragma comment(lib, "../../pcap/lib/wpcap.lib")

pcap_t* g_hPcap = NULL;

long	g_nKeepRunning = 0;
HANDLE	g_appThread = NULL;
DWORD WINAPI AppThread(void* data);

extern long g_ipIsReady;

HANDLE	g_dmaQLock = NULL;
long	g_qLength = 0;
struct packet_wrapper* g_dmaQueue = NULL;

void DMA_push(const struct pcap_pkthdr* packet_header, const u_char* packet)
{
	struct packet_wrapper* newPacket = (struct packet_wrapper*)malloc(sizeof(struct packet_wrapper));
	if (newPacket == NULL)
		return;
	newPacket->packet = malloc(packet_header->caplen);
	if (newPacket->packet == NULL)
	{
		free(newPacket);
		return;
	}
	newPacket->next = NULL;
	newPacket->len = packet_header->caplen;

	memcpy(newPacket->packet, packet, packet_header->caplen);

	if (g_dmaQLock != NULL)
		WaitForSingleObject(g_dmaQLock, INFINITE);

	if (g_qLength < MAX_DMA_QUEUE)
	{
		g_qLength++;
		if (g_dmaQueue == NULL)
		{
			g_dmaQueue = newPacket;
		}
		else
		{
			struct packet_wrapper* prev = g_dmaQueue;
			while (prev->next != NULL)
				prev = prev->next;
			prev->next = newPacket;
		}
	}
	else
	{
		printf("Packet queue is full: length = %d\n", g_qLength);
	}

	if (g_dmaQLock != NULL)
		ReleaseMutex(g_dmaQLock);

	NotifyFromEthISR(); //send event to lwip thread
}

struct packet_wrapper* DMA_pop()
{
	struct packet_wrapper* pkt = NULL;

	if (g_dmaQLock != NULL)
		WaitForSingleObject(g_dmaQLock, INFINITE);

	if (g_dmaQueue != NULL)
	{
		pkt = g_dmaQueue;
		g_dmaQueue = g_dmaQueue->next;
		pkt->next = NULL; //just the first packet

		g_qLength--;
		//printf("Packet consumed: length = %d\n", g_qLength);
	}

	if (g_dmaQLock != NULL)
		ReleaseMutex(g_dmaQLock);

	return pkt;
}

void DMA_free(struct packet_wrapper* pkt)
{
	struct packet_wrapper* prev = pkt;
	while(prev != NULL)
	{
		struct packet_wrapper* next = prev->next;

		if (prev->packet != NULL)
			free(prev->packet);
		free(prev);

		prev = next;
	}
}

unsigned char* NIC_GetBuffer(int size)
{
	unsigned char* buf = malloc(size);
	//LogPrint(0, "NIC_GetBuffer...\r\n");
	return buf;
}

long NIC_Send(unsigned char *buf, int size)
{
	long nSent = -1;

	//LogPrint(0, "NIC_Send...\r\n");

	if ((g_hPcap != NULL) && (buf != NULL))
	{
		nSent = pcap_sendpacket(g_hPcap, buf, size);
		if (nSent == PCAP_ERROR)
		{
			LogPrint(0, "pcap_sendpacket failed: %s, len=%d\r\n", pcap_geterr(g_hPcap), size);
		}
	}

	if (buf != NULL)
		free(buf);

	//LogPrint(0, "NIC_Send done\r\n");

	return nSent;
}

int main()
{
	int i = 0;

	bpf_u_int32 ip;
	bpf_u_int32 subnet_mask;
	struct in_addr address;
	DWORD exitCode = 0;

	struct bpf_program filterCompile;
	char szError[PCAP_ERRBUF_SIZE];

	pcap_if_t *allDevices, *dev;

	char szHostName[256];
	char szHostIP[128];
	char szHostSubnet[128];

	char szFilter[512];

	memset(szHostName, 0, sizeof(szHostName));
	memset(szFilter, 0, sizeof(szFilter));

	if (pcap_findalldevs(&allDevices, szError) == -1)
	{
		fprintf(stderr, "Error in pcap_findalldevs: %s\n", szError);
		exit(1);
	}

	printf("\nHere is a list of available devices on your system:\n\n");
	for (dev = allDevices; dev != NULL; dev = dev->next)
	{
		char* pStr;
		pcap_addr_t *dev_addr;

		if ((dev->flags & PCAP_IF_LOOPBACK) != 0)
			continue;
		if ((dev->flags & PCAP_IF_UP) == 0)
			continue;
		if ((dev->flags & PCAP_IF_RUNNING) == 0)
			continue;

		printf("%d. %s\n", ++i, dev->description);

		if ((dev->description == NULL) || (strlen(dev->description) <= 0))
			continue;

		if (strstr(dev->description, "NdisWan") != NULL)
			continue;
		if (strstr(dev->description, "VMware") != NULL)
			continue;
		if (strstr(dev->description, "Oracle") != NULL)
			continue;

		for (dev_addr = dev->addresses; dev_addr != NULL; dev_addr = dev_addr->next) 
		{
			if (dev_addr->addr->sa_family == AF_INET && dev_addr->addr && dev_addr->netmask) 
			{
				printf("    Found a device %s on address %s with netmask %s, broadaddr %s\n", dev->name, 
					inet_ntoa(((struct sockaddr_in *)dev_addr->addr)->sin_addr),
					inet_ntoa(((struct sockaddr_in *)dev_addr->netmask)->sin_addr),
					inet_ntoa(((struct sockaddr_in *)dev_addr->broadaddr)->sin_addr));
				continue;
			}
		}

		if (-1 == pcap_lookupnet(dev->name, &ip, &subnet_mask, szError))
		{
			printf("%s\n", szError);
			continue;
		}

		address.s_addr = ip;
		pStr = inet_ntoa(address);
		if (pStr == NULL)
			continue;
		strcpy(szHostIP, pStr);

		if (strstr(szHostIP, "0.0.0.0") == szHostIP)
			continue;
		if (strstr(szHostIP, "127.0") == szHostIP)
			continue;
		if (strstr(szHostIP, "169.254") == szHostIP)
			continue;

		address.s_addr = subnet_mask;
		pStr = inet_ntoa(address);
		if (pStr == NULL)
			continue;
		strcpy(szHostSubnet, pStr);

		strcpy(szHostName, dev->name);

		printf("    Selected Host: %s\n", szHostName);
		printf("       IP address: %s, %08lX\n", szHostIP, ip);
		printf("      Subnet Mask: %s\n", szHostSubnet);
		break;
	}
	pcap_freealldevs(allDevices);

	g_hPcap = pcap_open(szHostName,
		65536,
		PCAP_OPENFLAG_PROMISCUOUS | PCAP_OPENFLAG_MAX_RESPONSIVENESS,
		100,
		NULL,
		szError);
	if (g_hPcap == NULL)
	{
		fprintf(stderr, "\nUnable to open the adapter.\n");
		return -2;
	}

	pcap_setdirection(g_hPcap, PCAP_D_INOUT);
	pcap_setmode(g_hPcap, MODE_MON);// MODE_CAPT);

	g_dmaQLock = CreateMutex(NULL, FALSE, NULL);

	InterlockedExchange(&g_nKeepRunning, 1);

	g_appThread  = CreateThread(NULL, 0, AppThread, NULL, 0, NULL);
	//g_mainThread = CreateThread(NULL, 0, LwipLoopThread, NULL, 0, NULL);
	if (g_appThread != NULL)
		printf("Please press ESC key to quit\n ");

	sys_init();

	while(g_appThread != NULL)
	{
		struct pcap_pkthdr* pkt_header;
		u_char*				pkt_data;

		int  result = pcap_next_ex(g_hPcap, &pkt_header, &pkt_data);
/*
		1  if the packet has been read without problems
		0  if the timeout set with pcap_open_live() has elapsed. In this case pkt_header and pkt_data don't point to a valid packet
		-1 if an error occurred
		-2 if EOF was reached reading from an offline capture
*/
		if (result == 1)
			DMA_push(pkt_header, pkt_data);

		{
			int key = 0;
			if (_kbhit()) //check key press
			{
				key = _getch();
				if (key == 27)
				{
					InterlockedExchange(&g_nKeepRunning, 1);
					Sleep(100);
					break;
				}
			}
		}
		if (!GetExitCodeThread(g_appThread, &exitCode))
			break;
		if ((exitCode & STILL_ACTIVE) == 0)
			break;
	}
	pcap_close(g_hPcap);

	if (g_appThread != NULL)
		CloseHandle(g_appThread);

	//free the packet queue
	while (TRUE)
	{
		struct packet_wrapper* pkt = NULL;

		pkt = DMA_pop();
		if (pkt == NULL)
			break;
		DMA_free(pkt);
	}
}

extern struct netif main_netif;
struct altcp_pcb *g_pcbListen80 = NULL;
struct altcp_pcb *g_pcbListen443 = NULL;

extern void tcp_kill_all(void);

DWORD WINAPI AppThread(void* data)
{
	InitDevInfo(); //initiate device settings

	WEB_fs_init();	//initiate file system in memory

	SetupHttpContext(); //initiate http receiving context

	LwipInit(); //initiate lwip stack and start dhcp

	while (g_nKeepRunning)
	{
		SessionClearAll();	//clear http session context

		while (g_ipIsReady == 0)
		{
			if (tcpip_inloop() > 0)
				continue;
			SessionCheck();
		}

		PrintLwipStatus();

		netbiosns_set_name("lwipSimu");// GetDeviceName());
		netbiosns_init();

		ssdpInit(&main_netif);

		g_pcbListen80  = HttpdInit(0, 80);
		g_pcbListen443 = HttpdInit(1, 443);

		LogPrint(0, "App Service started\r\n");

		PrintLwipStatus();

		while(TRUE)
		{
			if (g_ipIsReady == 0)
				break;

			if (tcpip_inloop() > 0)
				continue;

			SessionCheck();
		}

		if (g_pcbListen80 != NULL)
			HttpdStop(g_pcbListen80);

		if (g_pcbListen443 != NULL)
			HttpdStop(g_pcbListen443);

		LogPrint(0, "App Service stopped\r\n");

		ssdpDown();
		netbiosns_stop();

		PrintLwipStatus();

		tcp_kill_all();

		if (g_ipIsReady == 0)
			break;
	}

	LwipStop();
	return 0;
}
