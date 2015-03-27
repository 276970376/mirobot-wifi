/*
Cgi/template routines for the /wifi url.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */


#include <string.h>
#include <osapi.h>
#include "user_interface.h"
#include "mem.h"
#include "httpd.h"
#include "io.h"
#include "espmissingincludes.h"

//Enable this to disallow any changes in AP settings
//#define DEMO_MODE

//WiFi access point data
typedef struct {
	char ssid[32];
	char rssi;
	char enc;
} ApData;

//Scan result
typedef struct {
	char scanInProgress; //if 1, don't access the underlying stuff from the webpage.
	ApData **apData;
	int noAps;
} ScanResultData;

//Static scan status storage.
ScanResultData cgiWifiAps;

//Callback the code calls when a wlan ap scan is done. Basically stores the result in
//the cgiWifiAps struct.
void ICACHE_FLASH_ATTR wifiScanDoneCb(void *arg, STATUS status) {
	int n;
	struct bss_info *bss_link = (struct bss_info *)arg;
	os_printf("wifiScanDoneCb %d\n", status);
	if (status!=OK) {
		cgiWifiAps.scanInProgress=0;
		return;
	}

	//Clear prev ap data if needed.
	if (cgiWifiAps.apData!=NULL) {
		for (n=0; n<cgiWifiAps.noAps; n++) os_free(cgiWifiAps.apData[n]);
		os_free(cgiWifiAps.apData);
	}

	//Count amount of access points found.
	n=0;
	while (bss_link != NULL) {
		bss_link = bss_link->next.stqe_next;
		n++;
	}
	//Allocate memory for access point data
	cgiWifiAps.apData=(ApData **)os_malloc(sizeof(ApData *)*n);
	cgiWifiAps.noAps=n;
	os_printf("Scan done: found %d APs\n", n);

	//Copy access point data to the static struct
	n=0;
	bss_link = (struct bss_info *)arg;
	while (bss_link != NULL) {
		if (n>=cgiWifiAps.noAps) {
			//This means the bss_link changed under our nose. Shouldn't happen!
			//Break because otherwise we will write in unallocated memory.
			os_printf("Huh? I have more than the allocated %d aps!\n", cgiWifiAps.noAps);
			break;
		}
		//Save the ap data.
		cgiWifiAps.apData[n]=(ApData *)os_malloc(sizeof(ApData));
		cgiWifiAps.apData[n]->rssi=bss_link->rssi;
		cgiWifiAps.apData[n]->enc=bss_link->authmode;
		strncpy(cgiWifiAps.apData[n]->ssid, (char*)bss_link->ssid, 32);

		bss_link = bss_link->next.stqe_next;
		n++;
	}
	//We're done.
	cgiWifiAps.scanInProgress=0;
}


//Routine to start a WiFi access point scan.
static void ICACHE_FLASH_ATTR wifiStartScan() {
//	int x;
	if (cgiWifiAps.scanInProgress) return;
	cgiWifiAps.scanInProgress=1;
	wifi_station_scan(NULL, wifiScanDoneCb);
}

//This CGI is called from the bit of AJAX-code in wifi.tpl. It will initiate a
//scan for access points and if available will return the result of an earlier scan.
//The result is embedded in a bit of JSON parsed by the javascript in wifi.tpl.
int ICACHE_FLASH_ATTR cgiWiFiScan(HttpdConnData *connData) {
	int len;
	int i;
	char buff[1024];
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "application/json");
	httpdEndHeaders(connData);

	if (cgiWifiAps.scanInProgress==1) {
		//We're still scanning. Tell Javascript code that.
		len=os_sprintf(buff, "{\n \"result\": { \n\"inProgress\": \"1\"\n }\n}\n");
		httpdSend(connData, buff, len);
	} else {
		//We have a scan result. Pass it on.
		len=os_sprintf(buff, "{\n \"result\": { \n\"inProgress\": \"0\", \n\"APs\": [\n");
		httpdSend(connData, buff, len);
		if (cgiWifiAps.apData==NULL) cgiWifiAps.noAps=0;
		for (i=0; i<cgiWifiAps.noAps; i++) {
			//Fill in json code for an access point
			len=os_sprintf(buff, "{\"essid\": \"%s\", \"rssi\": \"%d\", \"enc\": \"%d\"}%s\n", 
					cgiWifiAps.apData[i]->ssid, cgiWifiAps.apData[i]->rssi, 
					cgiWifiAps.apData[i]->enc, (i==cgiWifiAps.noAps-1)?"":",");
			httpdSend(connData, buff, len);
		}
		len=os_sprintf(buff, "]\n}\n}\n");
		httpdSend(connData, buff, len);
		//Also start a new scan.
		wifiStartScan();
	}
	return HTTPD_CGI_DONE;
}

// This callback allows us to restart the system after we've responded nicely to the request
static void ICACHE_FLASH_ATTR resetTimerCb(void *arg) {
	system_restart();
}

int ICACHE_FLASH_ATTR cgiWifiSettings(HttpdConnData *connData) {
	if (connData->conn==NULL) return HTTPD_CGI_DONE;
	int len;
	bool restart = false;
	bool connect = false;
	char buff[128];
	static ETSTimer postTimer;
	//static struct ip_info ipInfo;
	static struct station_config stconf;
	static struct softap_config  apconf;

	// get the current configuration
	wifi_station_get_config(&stconf);
	wifi_softap_get_config(&apconf);

	// Configure the WiFi mode if it has been set
	os_printf("WUH?\n");
	len=httpdFindArg(connData->post->buff, "wifiMode", buff, sizeof(buff));
	os_printf("len: %d\n", len);
	if (len>0) {
		os_printf("cgiWifiSetMode: %s\n", buff);
		uint8 mode = atoi(buff);
		if(mode != wifi_get_opmode()){
			wifi_set_opmode(mode);
			restart = true;
		}
	}

	// Configure the WiFi client settings
	if(wifi_get_opmode() & 1){
		// Configure the SSID of the access point to connect to
		len = httpdFindArg(connData->post->buff, "clientSSID", buff, sizeof(buff));
		if(len>0){
			if(os_strcmp((const char *)stconf.ssid, buff) != 0){
				// the value has changed
				os_printf("Setting Client SSID: %s\n", buff);
				os_strncpy((char*)stconf.ssid, buff, 32);
				connect = true;
			}
		}

		// Configure the password to use to connect to the access point
		len = httpdFindArg(connData->post->buff, "clientPasswd", buff, sizeof(buff));
		if(len>0){
			if(os_strcmp((const char *)stconf.password, buff) != 0){
				// the value has changed
				os_printf("Setting Client Passwd: %s\n", buff);
				os_strncpy((char*)stconf.password, buff, 64);
				connect = true;
			}
		}

		// No point setting any of this at the moment because it's not persistent
		/*
		//clientDhcp
		len = httpdFindArg(connData->post->buff, "clientDhcp", buff, sizeof(buff));
		if(len>0){
			os_printf("Setting DHCP: %d\n", atoi(buff));
			if(atoi(buff) == 1){
				os_printf("Starting DHCP\n");
				if(wifi_station_dhcpc_status() == DHCP_STOPPED){
					wifi_station_dhcpc_start();
					restart = true;
				}
			}else{
				os_printf("Stopping DHCP\n");
				if(wifi_station_dhcpc_status() == DHCP_STARTED){
					wifi_station_dhcpc_stop();
					restart = true;
				}
			}
		}

		if(wifi_station_dhcpc_status() == DHCP_STOPPED){
			// get the current config
			wifi_get_ip_info(STATION_IF, &ipInfo);
			//clientIp
			len = httpdFindArg(connData->post->buff, "clientIp", buff, sizeof(buff));
			if(len>0){
				os_printf("Setting client IP: %s\n", buff);
				IP4_ADDR(&ipInfo.ip, 10, 10, 100, 254);
			}
			//clientGateway
			len = httpdFindArg(connData->post->buff, "clientGateway", buff, sizeof(buff));
			if(len>0){
				os_printf("Setting client GW: %s\n", buff);
				IP4_ADDR(&ipInfo.gw, 10, 10, 100, 1);
			}
			//clientNetmask
			len = httpdFindArg(connData->post->buff, "clientNetmask", buff, sizeof(buff));
			if(len>0){
				os_printf("Setting client NM: %s\n", buff);
				IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
			}
			wifi_set_ip_info(STATION_IF, &ipInfo);
		}
		wifi_station_set_config(&stconf);
	*/
	}

	if(wifi_get_opmode() & 2){
		//APAuth
		len = httpdFindArg(connData->post->buff, "APAuth", buff, sizeof(buff));
		if(len>0){
			os_printf("Setting APAuth: %d\n", atoi(buff));
			apconf.authmode = atoi(buff);
			restart = true;
		}
		//APChannel
		len = httpdFindArg(connData->post->buff, "APChannel", buff, sizeof(buff));
		if(len>0){
			os_printf("Setting Channel: %d\n", atoi(buff));
			apconf.channel = atoi(buff);
			restart = true;
		}
		//APSSID
		len = httpdFindArg(connData->post->buff, "APSSID", buff, sizeof(buff));
		if(len>0){
			os_printf("Setting AP SSID: %s\n", buff);
			os_strncpy((char*)apconf.ssid, buff, 32);
			restart = true;
		}
		wifi_softap_set_config(&apconf);
	}

	if(restart){
		//Schedule disconnect/connect
		os_timer_disarm(&postTimer);
		os_timer_setfn(&postTimer, resetTimerCb, NULL);
		//Set to 0 if you want to disable the actual reconnecting bit
		os_timer_arm(&postTimer, 500, 0);
	}else	if(connect){
		os_printf("Try to connect to AP %s pw %s\n", stconf.ssid, stconf.password);
		wifi_station_disconnect();
		ETS_UART_INTR_DISABLE();
		wifi_station_set_config(&stconf);
		ETS_UART_INTR_ENABLE();
		wifi_station_connect();
	}

	httpdSend(connData, "HTTP/1.0 204 No Content\r\nServer: esp8266-httpd/0.1\r\n\r\n", -1);
	return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR tplWlanInfo(HttpdConnData *connData, char *token, void **arg) {
	char buff[1024];
	static struct station_config stconf;
	struct ip_info ipconfig;
	struct softap_config ap_conf;

	if (token==NULL) return HTTPD_CGI_DONE;
	wifi_station_get_config(&stconf);

	buff[0] = 0;
	if (os_strcmp(token, "wifiMode")==0) {
		os_sprintf(buff, "%d", wifi_get_opmode());
	} else if (os_strcmp(token, "clientSSID")==0) {
		os_strcpy(buff, (char*)stconf.ssid);
	} else if (os_strcmp(token, "clientPasswd")==0) {
		os_strcpy(buff, (char*)stconf.password);
	} else if (os_strcmp(token, "clientIp")==0) {
		if(wifi_get_opmode() == SOFTAP_MODE) return HTTPD_CGI_DONE;
		wifi_get_ip_info(STATION_IF, &ipconfig);
		os_sprintf(buff, IPSTR, IP2STR(&ipconfig.ip));
	} else if (os_strcmp(token, "clientGateway")==0) {
		if(wifi_get_opmode() == SOFTAP_MODE) return HTTPD_CGI_DONE;
		wifi_get_ip_info(STATION_IF, &ipconfig);
		os_sprintf(buff, IPSTR, IP2STR(&ipconfig.gw));
	} else if (os_strcmp(token, "clientNetmask")==0) {
		if(wifi_get_opmode() == SOFTAP_MODE) return HTTPD_CGI_DONE;
		wifi_get_ip_info(STATION_IF, &ipconfig);
		os_sprintf(buff, IPSTR, IP2STR(&ipconfig.netmask));
	} else if (os_strcmp(token, "clientDhcp")==0) {
		os_sprintf(buff, (wifi_station_dhcpc_status() == DHCP_STARTED ? "true" : "false"));
	} else if (os_strcmp(token, "clientMac")==0) {
	  uint8 mac[6];
		wifi_get_macaddr(STATION_IF, mac);
		os_sprintf(buff, MACSTR, MAC2STR(mac));
	} else if (os_strcmp(token, "clientConnected")==0) {
		os_sprintf(buff, "%d", wifi_station_get_connect_status());
	} else if (os_strcmp(token, "APAuth")==0) {
		wifi_softap_get_config(&ap_conf);
		os_sprintf(buff, "%d", ap_conf.authmode);
	} else if (os_strcmp(token, "APChannel")==0) {
		wifi_softap_get_config(&ap_conf);
		os_sprintf(buff, "%d", ap_conf.channel);
	} else if (os_strcmp(token, "APSSID")==0) {
		wifi_softap_get_config(&ap_conf);
		os_sprintf(buff, "%s", ap_conf.ssid);
	}

	httpdSend(connData, buff, -1);
	return HTTPD_CGI_DONE;
}
