#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"

#include "ns3/nr-module.h"
#include "ns3/nr-point-to-point-epc-helper.h"

using namespace ns3;

int main (int argc, char *argv[])
{
  // ---- Simulation params ----
  uint16_t gNbNum = 1;
  uint16_t ueNum  = 2;
  double   centralFreq = 28e9;     // 28 GHz (FR2)
  double   bandwidth  = 100e6;     // 100 MHz
  uint8_t  numCc      = 1;         // one CC → one BWP
  uint8_t  numerology = 3;         // Use numerology instead of SCS enum (3 = 120 kHz)
  double   simTimeSec = 3.0;
  bool     logging    = false;

  CommandLine cmd;
  cmd.AddValue ("gNbNum", "Number of gNBs", gNbNum);
  cmd.AddValue ("ueNum", "Number of UEs", ueNum);
  cmd.AddValue ("centralFreq", "Central frequency (Hz)", centralFreq);
  cmd.AddValue ("bandwidth", "Channel bandwidth (Hz)", bandwidth);
  cmd.AddValue ("numCc", "Number of component carriers per band", numCc);
  cmd.AddValue ("simTime", "Simulation time (s)", simTimeSec);
  cmd.AddValue ("logging", "Enable some logs", logging);
  cmd.Parse (argc, argv);

  if (logging) {
    LogComponentEnable ("UdpClient", LOG_LEVEL_INFO);
    LogComponentEnable ("UdpServer", LOG_LEVEL_INFO);
  }

  // ---- Nodes ----
  NodeContainer gNbNodes;  gNbNodes.Create (gNbNum);
  NodeContainer ueNodes;   ueNodes.Create (ueNum);

  NodeContainer remoteHostContainer; 
  remoteHostContainer.Create (1);
  Ptr<Node> remoteHost = remoteHostContainer.Get (0);

  // ---- EPC / Core ----
  Ptr<NrPointToPointEpcHelper> epcHelper = CreateObject<NrPointToPointEpcHelper> ();
  Ptr<Node> pgw = epcHelper->GetPgwNode ();

  // PGW <-> RemoteHost wired link
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  p2p.SetChannelAttribute ("Delay",    StringValue ("1ms"));
  NetDeviceContainer internetDevs = p2p.Install (pgw, remoteHost);

  InternetStackHelper internet;
  internet.Install (remoteHostContainer);         // IP stack on remote host
  internet.Install (ueNodes);                     // UEs get IPs via EPC

  Ipv4AddressHelper ipv4h;
  ipv4h.SetBase ("1.0.0.0", "255.0.0.0");
  Ipv4InterfaceContainer ifaces = ipv4h.Assign (internetDevs);
  Ipv4Address pgwAddr       = ifaces.GetAddress (0);
  Ipv4Address remoteHostIp  = ifaces.GetAddress (1);

  // RemoteHost route to UE net (7.0.0.0/8 default from EPC)
  Ipv4StaticRoutingHelper rtHelper;
  Ptr<Ipv4StaticRouting> rhRoute = rtHelper.GetStaticRouting (remoteHost->GetObject<Ipv4> ());
  rhRoute->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"), pgwAddr, 1);

  // ---- Mobility BEFORE device install ----
  MobilityHelper mob;

  Ptr<ListPositionAllocator> gnbPos = CreateObject<ListPositionAllocator> ();
  gnbPos->Add (Vector (0.0, 0.0, 10.0));   // gNB at 10 m height
  mob.SetPositionAllocator (gnbPos);
  mob.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mob.Install (gNbNodes);

  Ptr<ListPositionAllocator> uePos = CreateObject<ListPositionAllocator> ();
  for (uint32_t i = 0; i < ueNum; ++i) {
    uePos->Add (Vector (5.0 + 10.0 * i, 0.0, 1.5));
  }
  mob.SetPositionAllocator (uePos);
  mob.Install (ueNodes);

  // ---- NR Helper and BWPs (updated API) ----
  Ptr<NrHelper> nr = CreateObject<NrHelper> ();
  nr->SetEpcHelper (epcHelper);

  // Create operation bands using the new API
  CcBwpCreator ccBwpCreator;
  CcBwpCreator::SimpleOperationBandConf bandConf (centralFreq, bandwidth, numCc, numerology);
  OperationBandInfo band = ccBwpCreator.CreateOperationBandContiguousCc (bandConf);

  // Install NR devices
  NetDeviceContainer gnbDevs = nr->InstallGnbDevice (gNbNodes, band);
  NetDeviceContainer ueDevs  = nr->InstallUeDevice  (ueNodes,  band);

  // Attach & bearer
  nr->AttachToClosestGnb (ueDevs, gnbDevs);
  NrEpsBearer bearer (NrEpsBearer::NGBR_LOW_LAT_EMBB);
  nr->ActivateDataRadioBearer (ueDevs, bearer);

  // EPC assigns UE IPs
  epcHelper->AssignUeIpv4Address (ueDevs);

  // ---- UL traffic: UEs -> RemoteHost (UDP) ----
  uint16_t ulPort = 4444;
  UdpServerHelper ulServer (ulPort);
  ApplicationContainer serverApps = ulServer.Install (remoteHostContainer);
  serverApps.Start (Seconds (0.2));
  serverApps.Stop  (Seconds (simTimeSec));

  for (uint32_t i = 0; i < ueDevs.GetN (); ++i) {
    UdpClientHelper ulClient (remoteHostIp, ulPort);
    ulClient.SetAttribute ("MaxPackets", UintegerValue (0));      // unlimited
    ulClient.SetAttribute ("Interval",   TimeValue (MilliSeconds (2)));
    ulClient.SetAttribute ("PacketSize", UintegerValue (512));
    ApplicationContainer c = ulClient.Install (ueNodes.Get (i));
    c.Start (Seconds (0.4 + 0.05 * i));
    c.Stop  (Seconds (simTimeSec));
  }

  Simulator::Stop (Seconds (simTimeSec));
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}