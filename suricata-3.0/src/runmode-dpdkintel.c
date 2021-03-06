#define _GNU_SOURCE

#include "runmode-dpdkintel.h"
#include "source-dpdkintel.h"
#include "util-dpdk-config.h"
#include "util-dpdk-setup.h"

#include "suricata-common.h"
#include "tm-threads.h"
#include "conf.h"
#include "runmodes.h"
#include "log-httplog.h"
#include "output.h"

#include "alert-fastlog.h"
#include "alert-prelude.h"
#include "alert-unified2-alert.h"
#include "alert-debuglog.h"

#include "util-debug.h"
#include "util-time.h"
#include "util-cpu.h"
#include "util-affinity.h"
#include "util-runmodes.h"
#include "util-device.h"

#include "decode.h"



#define DPDKINTEL_RUNMODE_WORKERS 1

/* E X T E R N */

extern file_config_t file_config;
extern DpdkIntelPortMap portMap [16];

static const char *default_mode = NULL;

static int32_t getDpdkDeviceList(void)
{
    static uint8_t interfaceCount = 0;

    SCLogDebug("Interface to fetch %d\n", interfaceCount);
    return interfaceCount++;
}

const char *RunModeDpdkIntelGetDefaultMode(void)
{
    return default_mode;
}

void RunModeDpdkIntelRegister(void)
{
#ifdef HAVE_DPDKINTEL
    default_mode = "workers";
    RunModeRegisterNewRunMode(RUNMODE_DPDKINTEL, "workers",
            "Workers DpdkIntel mode, each thread does all"
            " tasks from decoding to logging. Acquistion is "
            " done by seperate core per interface",
            RunModeDpdkIntelWorkers);
#endif /* HAVE_DPDKINTEL */
}

int DpdkIntelRegisterDeviceStreams()
{
    int use_all_streams;

    /* ToDo: stream configruation in DPDK ?? */
    use_all_streams = 1;

    if (use_all_streams)
    {
        SCLogInfo("Using All DpdkIntel Streams");
    }
    else
    {
        SCLogInfo("Using Selected DpdkIntel Streams");
    }
    return 0;
}

void ParseDpdkConfig(void)
{
    char *oface = NULL, *iface = NULL, *val = NULL;
    uint8_t portIndex = 0, portTotal = rte_eth_dev_count_avail(), portInOutSet = 0x00;
    DPDKINTEL_GENCFG.Port = 0;
    uint32_t portMapIndex = 0;
    int index = 0;
    struct rte_eth_dev_info dev_info;
    ConfNode *ifnode, *ifroot;

    ConfNode *dpdkIntel_node = ConfGetNode("dpdkintel");
    if (dpdkIntel_node == NULL) {
        SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "Unable to find dpdkintel config using default value");
        return;
    }

    if (ConfGet("dpdkintel.opmode", &val) != 1) {
        SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "Unable to find opmode");
        return;
    }
    if (strlen(val) != 3)
        DPDKINTEL_GENCFG.OpMode = BYPASS;
    else if (strcasecmp(val, "ips") == 0)
        DPDKINTEL_GENCFG.OpMode = IPS;
    else if (strcasecmp(val, "ids") == 0)
        DPDKINTEL_GENCFG.OpMode = IDS;
    else
        DPDKINTEL_GENCFG.OpMode = BYPASS;

    SCLogDebug("DPDK Opmode set to %u", DPDKINTEL_GENCFG.OpMode);

    ifnode = ConfGetNode("dpdkintel.inputs");
    if (ifnode == NULL) {
        SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "Unable to find interface");
        return;
    }
    /* get for each device in list to populate the copy interface */
    SCLogDebug(" Dev Count: %d", LiveGetDeviceCount());
    for (index = 0; index < LiveGetDeviceCount(); index++)
    {
        iface = LiveGetDeviceName(index);
        SCLogInfo(" Device Name: %s", iface);
        ifroot = ConfNodeLookupKeyValue(ifnode, "interface", iface);
        if (ifroot == NULL) {
            SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "Unable to find device %s",
                                                        iface);
            return;
        }
        /* check if interface lies within limits */
        if( (atoi(iface) < 0) && (atoi(iface) > portTotal)) {
                SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "iface %s does not lie within DPDK total ports %d",
                                                            iface, portTotal);
                return;
        }

        if (DPDKINTEL_GENCFG.OpMode != IDS) {
            if (ConfGetChildValue(ifroot, "copy-interface", &oface) != 1) {
                SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "Unable to find cpy-ifce for device %s",
                                                            iface);
                return;

            }
            SCLogInfo(" copy-interface %s", oface);

            if (!strcmp(iface, oface)) {
                SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "in and out interface cannot be same %s <--> %s",
                                                            iface, oface);
                return;
            }
            /* check if copy-interface lies within limits */
            if ((atoi(oface) < 0) && (atoi(oface) > portTotal)) {
                SCLogError(SC_ERR_DPDKINTEL_CONFIG_FAILED, "oface %s does not lie within DPDK total ports %d",
                                                            oface, portTotal);
                return;
            }
        }

        {
	portMap [portMapIndex].inport = atoi(iface);
	portInOutSet |= 0x01;
	if ((DPDKINTEL_GENCFG.OpMode != IDS)) {
		/* PortMap structure update with outport */
		portMap [portMapIndex].outport = atoi(oface);
		portInOutSet |= 0x02;
	}

            if ((DPDKINTEL_GENCFG.OpMode != IDS) && (portInOutSet == 0x03))
            {
                //portMap [portMapIndex].ringid = DPDKINTEL_GENCFG.Port;
                portMap [portMapIndex].ringid = portMap [portMapIndex].inport;
                DPDKINTEL_GENCFG.Port += 1;
                SCLogInfo("PortMap : Inport: %u OutPort: %u  ringid %u",
                         portMap [portMapIndex].inport,
                         portMap [portMapIndex].outport,
                         portMap [portMapIndex].ringid
                         );
                portInOutSet = 0x00;
                portMapIndex++;
            }
            else if ((DPDKINTEL_GENCFG.OpMode == IDS) && (portInOutSet == 0x01))
            {
                //portMap [portMapIndex].ringid = DPDKINTEL_GENCFG.Port;
                portMap [portMapIndex].ringid = portMap [portMapIndex].inport;
                DPDKINTEL_GENCFG.Port += 1;
                SCLogInfo("PortMap : Inport: %u OutPort: %u  ringid %u",
                         portMap [portMapIndex].inport,
                         portMap [portMapIndex].outport,
                         portMap [portMapIndex].ringid
                         );
                portInOutSet = 0x00;
                portMapIndex++;
            }
            else
               SCLogInfo("never happen!!!!");

        }
    }
    SCLogDebug("total port count = %d",DPDKINTEL_GENCFG.Port);
}


void *DpdkIntelConfigParser(const char *device) 
{
    //int inputDevice = 0;
    int deviceIndex = 0;
    char intf[5];

    DpdkIntelIfaceConfig_t *dpdkIntelConf = SCMalloc(sizeof(DpdkIntelIfaceConfig_t));
    if (unlikely(dpdkIntelConf == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate memory for dev %s ", device);
        return NULL;
    }
    memset (dpdkIntelConf, 0, sizeof(DpdkIntelIfaceConfig_t));

    SC_ATOMIC_INIT(dpdkIntelConf->ref);
    (void) SC_ATOMIC_ADD(dpdkIntelConf->ref, 1);

    //inputDevice = getDpdkDeviceList();
    deviceIndex = getDpdkDeviceList();

    dpdkIntelConf->cluster_id = 1;
    dpdkIntelConf->cluster_type = PACKET_FANOUT_HASH;

    //sprintf(intf, "%d", inputDevice);
    sprintf(intf, "%d", portMap[deviceIndex].inport);
    memcpy(dpdkIntelConf->iface, intf, 1);

    //sprintf(intf, "%d", DPDKINTEL_PRTCFG[inputDevice - 1].portMap);
    sprintf(intf, "%d", portMap[deviceIndex].outport);
    //memcpy(dpdkIntelConf->outIface, intf, 1); 
    dpdkIntelConf->outIface = SCStrdup(intf); 

    dpdkIntelConf->ringBufferId = portMap[deviceIndex].ringid;
    dpdkIntelConf->flags = 0;
    dpdkIntelConf->bpfFilter = NULL;
    dpdkIntelConf->threads = 1;
   // dpdkIntelConf->outIface = NULL;

    /* ToDo: bpf filter supprt for DPDK ??? */

    SC_ATOMIC_RESET(dpdkIntelConf->ref);
    (void) SC_ATOMIC_ADD(dpdkIntelConf->ref, dpdkIntelConf->threads);

    /* ToDo: support for cluster id and type (RR & Flow) in DPDK ??? */
    dpdkIntelConf->cluster_type = PACKET_FANOUT_CPU;

    dpdkIntelConf->promiscous = 1;

    dpdkIntelConf->checksumMode = CHECKSUM_VALIDATION_ENABLE; /* using itnerface checksum offlaoding */
    SCLogDebug(" ----------- completed parse config for dpdk");
    return dpdkIntelConf;
}

int DpdkIntelGetThreadsCount(void *conf __attribute__((unused))) 
{
    return 1;
}

int RunModeDpdkIntelWorkers(void) 
{
    int ret;

    SCEnter();

    RunModeInitialize();
    TimeModeSetLive();

    ret = RunModeSetLiveCaptureWorkers(DpdkIntelConfigParser, DpdkIntelGetThreadsCount,
                                       "DpdkIntelReceive", "DpdkIntelDecode",
                                       "RxDPDKINTEL", "pktacqloop");

    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    return 0;
}

