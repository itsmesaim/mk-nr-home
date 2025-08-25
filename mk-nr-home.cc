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
  uint16_t ueNum = 2;
  double centralFreq = 28e9;       // 28 GHz
  double bandwidth  = 100e6;       // 100 MHz
  uint8_t numCc     = 1;           // one CC → one BWP
  uint32_t numerology = 1;         // μ=1 ≈ 30 kHz SCS
  double simTimeSec = 3.0;         // total sim time
  bool logging = false;

  CommandLine cmd;
  cmd.AddValue ("gNbNum", "Number of gNBs", gNbNum);
  cmd.AddValue ("ueNum", "Number of UEs", ueNum);
  cmd.AddValue ("centralFreq", "Central frequency (Hz)", centralFreq);
  cmd.AddValue ("bandwidth", "Channel bandwidth (Hz)", bandwidth);
  cmd.AddValue ("numCc", "Number of component carriers per band", numCc);
  cmd.AddValue ("numerology", "NR numerology (0..4)", numerology);
  cmd.AddValue ("simTime", "Simulation time (s)", simTimeSec);
  cmd.AddValue ("logging", "Enable some logs", logging);
  cmd.Parse (argc, argv);

  if (logging)
  {
    LogComponentEnable ("UdpClient", LOG_LEVEL_INFO);
    LogComponentEnable ("UdpServer", LOG_LEVEL_INFO);
  }

  // ---- Create nodes ----
  NodeContainer gNbNodes;  gNbNodes.Create (gNbNum);
  NodeContainer ueNodes;   ueNodes.Create (ueNum);

  // Remote host (the "Internet" beyond the PGW)
  NodeContainer remoteHostContainer; 
  remoteHostContainer.Create (1);
  Ptr<Node> remoteHost = remoteHostContainer.Get (0);

  // ---- EPC / Core ----
  Ptr<NrPointToPointEpcHelper> epcHelper = CreateObject<NrPointToPointEpcHelper> ();
  Ptr<Node> pgw = epcHelper->GetPgwNode ();

  // Internet between PGW <-> RemoteHost
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("1ms"));
  NetDeviceContainer internetDevs = p2p.Install (pgw, remoteHost);

  InternetStackHelper internet;
  internet.Install (remoteHostContainer);         // IP stack on remote host
  internet.Install (ueNodes);                     // IP stack on UEs (EPC assigns UE IPs)

  Ipv4AddressHelper ipv4h;
  ipv4h.SetBase ("1.0.0.0", "255.0.0.0");        // simple /8 for PGW<->RemoteHost link
  Ipv4InterfaceContainer ifaces = ipv4h.Assign (internetDevs);
  Ipv4Address pgwAddr       = ifaces.GetAddress (0);  // on PGW
  Ipv4Address remoteHostIp  = ifaces.GetAddress (1);  // on RemoteHost

  // Route on RemoteHost to UE subnet (EPC gives UEs 7.0.0.0/8 by default)
  Ipv4StaticRoutingHelper rtHelper;
  Ptr<Ipv4StaticRouting> rhRoute = rtHelper.GetStaticRouting (remoteHost->GetObject<Ipv4> ());
  rhRoute->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"), pgwAddr, 1);

  // =========================
  // IMPORTANT: Set mobility BEFORE installing NR devices
  // =========================

  // gNB at origin; UEs on a line at y=0, spaced by 10 m
  MobilityHelper mob;

  Ptr<ListPositionAllocator> gnbPos = CreateObject<ListPositionAllocator> ();
  gnbPos->Add (Vector (0.0, 0.0, 10.0));              // 10 m high
  mob.SetPositionAllocator (gnbPos);
  mob.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mob.Install (gNbNodes);

  Ptr<ListPositionAllocator> uePos = CreateObject<ListPositionAllocator> ();
  for (uint32_t i = 0; i < ueNum; ++i)
  {
    uePos->Add (Vector (5.0 + 10.0 * i, 0.0, 1.5));
  }
  mob.SetPositionAllocator (uePos);
  mob.Install (ueNodes);

  // ---- NR Helper and spectrum/BWPs (new v4.x API) ----
  Ptr<NrHelper> nr = CreateObject<NrHelper> ();
  nr->SetEpcHelper (epcHelper);

  // Set numerology BEFORE creating bandwidth parts
  nr->SetGnbPhyAttribute ("Numerology", UintegerValue (numerology));
  nr->SetUePhyAttribute ("Numerology", UintegerValue (numerology));

  std::vector<CcBwpCreator::SimpleOperationBandConf> bands;
  bands.emplace_back (centralFreq, bandwidth, numCc);

  // Create BWPs & channels (no InitializeOperationBand in v4.x)
  auto [totalBw, allBwps] = nr->CreateBandwidthParts (bands);

  // ---- gNB / UE devices (mobility is already set) ----
  NetDeviceContainer gnbDevs = nr->InstallGnbDevice (gNbNodes, allBwps);
  NetDeviceContainer ueDevs  = nr->InstallUeDevice  (ueNodes,  allBwps);

  // Remove the old numerology setting code since we set it before device installation
  /*
  // Set numerology μ on BWP 0 for all devices (your old 30 kHz intent)
  for (uint32_t i = 0; i < gnbDevs.GetN (); ++i)
  {
    Ptr<NrGnbPhy> gnbPhy0 = NrHelper::GetGnbPhy (gnbDevs.Get (i), 0);
    gnbPhy0->SetAttribute ("Numerology", UintegerValue (numerology));
  }
  for (uint32_t i = 0; i < ueDevs.GetN (); ++i)
  {
    Ptr<NrUePhy> uePhy0 = NrHelper::GetUePhy (ueDevs.Get (i), 0);
    uePhy0->SetAttribute ("Numerology", UintegerValue (numerology));
  }
  */

  // ---- Attach UEs and set bearer ----
  nr->AttachToClosestGnb (ueDevs, gnbDevs);  // initial association
  NrEpsBearer bearer (NrEpsBearer::NGBR_LOW_LAT_EMBB);
  nr->ActivateDataRadioBearer (ueDevs, bearer);

  // ---- UL traffic: UEs -> RemoteHost (UDP) ----
  uint16_t ulPort = 4444;
  UdpServerHelper ulServer (ulPort);
  ApplicationContainer serverApps = ulServer.Install (remoteHostContainer);
  serverApps.Start (Seconds (0.2));
  serverApps.Stop  (Seconds (simTimeSec));

  for (uint32_t i = 0; i < ueDevs.GetN (); ++i)
  {
    UdpClientHelper ulClient (remoteHostIp, ulPort);
    ulClient.SetAttribute ("MaxPackets", UintegerValue (0));      // unlimited
    ulClient.SetAttribute ("Interval",   TimeValue (MilliSeconds (2)));
    ulClient.SetAttribute ("PacketSize", UintegerValue (512));
    ApplicationContainer c = ulClient.Install (ueNodes.Get (i));
    c.Start (Seconds (0.4 + 0.05 * i));
    c.Stop  (Seconds (simTimeSec));
  }

  // ---- Run ----
  Simulator::Stop (Seconds (simTimeSec));
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}