/*
GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
https://github.com/Xorlent/The-26-Dollar-Honeypot

Libraries and supporting code incorporates other licenses, see https://github.com/Xorlent/The-26-Dollar-Honeypot/blob/main/LICENSE-3RD-PARTY.md
*/
////////------------------------------------------- CONFIGURATION SETTINGS AREA --------------------------------------------////////

const uint8_t hostName[] = "ESPoE-Honeypot"; // Set hostname, no spaces, no domain name per RFC 3164

// Ethernet configuration:
IPAddress ip(192, 168, 12, 61); // Set device IP address.
IPAddress gateway(192, 168, 12, 1); // Set default gateway IP address.
IPAddress subnet(255, 255, 255, 0); // Set network subnet mask.
IPAddress dns1(9, 9, 9, 9); // Primary DNS
IPAddress dns2(149, 112, 112, 112); // Secondary DNS

IPAddress syslogSvr(192, 168, 1, 100); // Set Syslog collector IP address.
const uint16_t syslogPort = 514; // Set Syslog collector UDP port.

//Select your NTP server info by configuring and uncommenting ONLY ONE line below:
//IPAddress ntpSvr(192, 168, 1, 2); // Set internal NTP server IP address.
const char* ntpSvr = "pool.ntp.org"; // Or set a NTP DNS server hostname.

//Choose your honeypot port personality by uncommenting ONLY ONE line below:
//If you want to set a different list of ports, this is fine, but the list MUST NOT exceed 14 entries.
const uint16_t honeypotTCPPorts[14] = {22,23,80,389,443,445,636,1433,1521,3268,3306,5432,8080,27017}; // Common enterprise network ports
//const uint16_t honeyportTCPPorts[14] = {22,23,80,102,473,502,2222,18245,18246,20000,28784,34962,44818,57176}; // Common OT/SCADA environment ports

////////--------------------------------------- DO NOT EDIT ANYTHING BELOW THIS LINE ---------------------------------------////////

#include <ETH.h>
#include <NTP.h>
#include <sys/socket.h>

#define ETH_ADDR        1
#define ETH_POWER_PIN   5
#define ETH_TYPE        ETH_PHY_IP101
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18

////////---------------------------------------        Create runtime objects        ---------------------------------------////////

// Syslog snippets used to build a full message.
const uint8_t syslogPri[] = "<116>";
const uint8_t syslogSvc[] = " TCP/";
const uint8_t syslogMsg[] = ":Connection from ";

const uint8_t honeypotNumPorts = sizeof(honeypotTCPPorts)/sizeof(honeypotTCPPorts[0]);

// UDP for Syslog event transmission and NTP time sync
WiFiUDP syslog;
NTP ntp(syslog);

// Globals used to manage and scan ports for client connections
int sockfd[honeypotNumPorts];
int maxSockfd = 0;
int newSocket;
fd_set listeners;  // list of all active socketfds
int activity;
struct sockaddr_in address; // client address

////////---------------------------------------     End create runtime objects     ---------------------------------------////////

// Takes a port number as an argument and returns the socket file descriptor for the established listener
int listener(uint16_t portNum)
{
  int listenersockfd;
  struct sockaddr_in my_addr;
  
  listenersockfd = socket(AF_INET, SOCK_STREAM, 0);
  
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons(portNum);
  my_addr.sin_addr.s_addr = INADDR_ANY;

  bind(listenersockfd, (struct sockaddr*)&my_addr, sizeof(struct sockaddr));
  Serial.print(portNum);
  Serial.print(" bound.");
  Serial.println();
  listen(listenersockfd,2);
  Serial.print("Now listening: ");
  Serial.print(listenersockfd);
  Serial.println();

  return listenersockfd;
}

// Takes the index of the port to report activity on (see honeypotTCPPorts), builds and sends the syslog event.  BSD / RFC 3164 format.
void logEvent(int fdindex)
{
  ntp.update(); // Get current time
  uint8_t DTS[17];
  memcpy(DTS, ntp.formattedTime("%b %d %T "), 16);
  Serial.println(ntp.formattedTime("%b %d %T "));
  char portString[6] = "";
  itoa(honeypotTCPPorts[fdindex],portString,10);
  int eventBytes = (sizeof(syslogPri) - 1 + sizeof(DTS) - 1 + sizeof(hostName) - 1 + sizeof(syslogSvc) - 1 + strlen(portString) + sizeof(syslogMsg) - 1 + 15);
  uint8_t eventData[eventBytes];
  memcpy(eventData, syslogPri, sizeof(syslogPri)-1);
  memcpy(eventData+sizeof(syslogPri)-1, DTS, sizeof(DTS)-1);
  memcpy(eventData+sizeof(syslogPri)-1+sizeof(DTS)-1, hostName, sizeof(hostName)-1);
  memcpy(eventData+sizeof(syslogPri)-1+sizeof(DTS)-1+sizeof(hostName)-1, syslogSvc, sizeof(syslogSvc)-1);
  memcpy(eventData+sizeof(syslogPri)-1+sizeof(DTS)-1+sizeof(hostName)-1+sizeof(syslogSvc)-1, portString, strlen(portString));
  memcpy(eventData+sizeof(syslogPri)-1+sizeof(DTS)-1+sizeof(hostName)-1+sizeof(syslogSvc)-1+strlen(portString), syslogMsg, sizeof(syslogMsg)-1);
  memcpy(eventData+sizeof(syslogPri)-1+sizeof(DTS)-1+sizeof(hostName)-1+sizeof(syslogSvc)-1+strlen(portString)+sizeof(syslogMsg)-1, inet_ntoa(address.sin_addr), 15);

  syslog.beginPacket(syslogSvr,syslogPort);
  syslog.write(eventData, eventBytes);
  syslog.endPacket();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
      delay(100);
  }

  // Initialize Ethernet
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_TYPE);
  ETH.config(ip, gateway, subnet, dns1, dns2);
  while(!ETH.linkUp())
  {
    delay(1000);
  }

  ntp.begin(ntpSvr); // We query the NTP server for the current time immediately before sending a Syslog message to ensure accuracy
  ntp.updateInterval(600000);

  for(int i=0;i<honeypotNumPorts;i++)
  {
    sockfd[i] = listener(honeypotTCPPorts[i]); // Start listeners on all specified ports
  }
}

void loop() {
  // Prepare for a round of client connection checks
  FD_ZERO(&listeners); // Zero the file descriptor set
  maxSockfd = 0; // Zero the maximum file descriptor value

  for(int i=0;i<honeypotNumPorts;i++)
  {
    FD_SET(sockfd[i], &listeners); // Add socket to select list
    if(sockfd[i] > maxSockfd)
    {
      maxSockfd = sockfd[i]; // Update maximum file descriptor value if necessary
    }
  }

  activity = select(maxSockfd+1 ,&listeners ,NULL ,NULL ,NULL); // Begin to monitor file descriptors
  if ((activity < 0) && (errno!=EINTR)) 
  { 
    Serial.print("FATAL: Select error."); 
  }

  // Iterate through the file descriptor set and check for initiated connections
  for(int i=0;i<honeypotNumPorts;i++)
  {
    if (FD_ISSET(sockfd[i], &listeners)) 
    { 
      int addrlen;
      // Get this socket so we can fetch the source IP from the connection
      if ((newSocket = accept(sockfd[i], (struct sockaddr *)&address, (socklen_t*)&addrlen))<0) 
      { 
        perror("accept"); 
        exit(EXIT_FAILURE); 
      } 

      Serial.printf("New connection, IP : %s , Port : %d \n" , inet_ntoa(address.sin_addr) , honeypotTCPPorts[i]); 
      // Close the client's connection
      close(newSocket);
      logEvent(i);
      break;
    } 
  }
}