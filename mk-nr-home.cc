#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/netanim-module.h"

// NR / 5G-LENA
#include "ns3/nr-module.h"
#include "ns3/antenna-module.h"
#include "ns3/mobility-module.h"
#include "ns3/epc-helper.h"
#include "ns3/point-to-point-epc-helper.h"

using namespace ns3;

/*
 Goal:
 - Keep your four LANs (Home, Office, Uni, IoT) the same
 - Replace HomeGW <-> CORE wired link with an NR air link:
      HomeGW acts as a UE
      New gNB node talks to that UE
      gNB is connected to EPC/PGW
      PGW connects to CORE via P2P (so the rest of your city still routes via CORE)
 - Apps and tracing remain the same
*/

static void SetPos(Ptr<Node> n, double x, double y) {
  // give nodes a constant position so NetAnim is happy
  Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
  pos->Add(Vector(x,y,0));
  MobilityHelper mob;
  mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  NodeContainer one(n);
  mob.SetPositionAllocator(pos);
  mob.Install(one);
}

int main(int argc, char* argv[]) {
  Time::SetResolution(Time::NS);
  LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
  LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);

  // ----------- City layout -----------
  Ptr<Node> core = CreateObject<Node>();         // central core
  NodeContainer homeHosts; homeHosts.Create(2);
  Ptr<Node> homeGw = CreateObject<Node>();       // will be a UE
  NodeContainer officeHosts; officeHosts.Create(2);
  Ptr<Node> officeGw = CreateObject<Node>();
  NodeContainer uniHosts; uniHosts.Create(2);
  Ptr<Node> uniGw = CreateObject<Node>();
  NodeContainer iotHosts; iotHosts.Create(2);
  Ptr<Node> iotGw = CreateObject<Node>();

  InternetStackHelper stack;
  stack.Install(core);
  stack.Install(homeGw);   stack.Install(homeHosts);
  stack.Install(officeGw); stack.Install(officeHosts);
  stack.Install(uniGw);    stack.Install(uniHosts);
  stack.Install(iotGw);    stack.Install(iotHosts);

  // LAN helpers
  CsmaHelper csma; csma.SetChannelAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
  csma.SetChannelAttribute("Delay", TimeValue(MilliSeconds(0.5)));
  PointToPointHelper p2p; p2p.SetDeviceAttribute("DataRate", StringValue("50Mbps"));
  p2p.SetChannelAttribute("Delay", StringValue("2ms"));

  // ---- Build LANs (CSMA) ----
  NodeContainer homeLan;   homeLan.Add(homeGw);   homeLan.Add(homeHosts);
  NodeContainer officeLan; officeLan.Add(officeGw); officeLan.Add(officeHosts);
  NodeContainer uniLan;    uniLan.Add(uniGw);     uniLan.Add(uniHosts);
  NodeContainer iotLan;    iotLan.Add(iotGw);     iotLan.Add(iotHosts);

  NetDeviceContainer homeLanDevs   = csma.Install(homeLan);
  NetDeviceContainer officeLanDevs = csma.Install(officeLan);
  NetDeviceContainer uniLanDevs    = csma.Install(uniLan);
  NetDeviceContainer iotLanDevs    = csma.Install(iotLan);

  // ---- NR EPC stack (for Home <-> Core via air) ----
  // EPC with PGW
  Ptr<PointToPointEpcHelper> epc = CreateObject<PointToPointEpcHelper>();
  Ptr<Node> pgw = epc->GetPgwNode();

  // gNB helper & config
  Ptr<NrHelper> nr = CreateObject<NrHelper>();
  // Minimal config: single CC, FR1-like bandwidth
  uint16_t gNbNum = 1;
  uint16_t ueNum  = 1;

  // Make gNB node (separate from CORE for clarity)
  NodeContainer gNbNodes; gNbNodes.Create(gNbNum);
  NodeContainer ueNodes;  ueNodes.Add(homeGw); // Home GW acts as the UE

  // Mobility for gNB/UE
  MobilityHelper mob;
  mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mob.Install(gNbNodes);
  mob.Install(ueNodes);
  // Positions for NetAnim
  SetPos(core,     50, 40);
  SetPos(gNbNodes.Get(0), 40, 40);  // near CORE
  SetPos(homeGw,   10, 60);
  for (uint32_t i=0;i<homeHosts.GetN();++i)  SetPos(homeHosts.Get(i), 5 + 8*i, 70);
  SetPos(officeGw, 90, 60);
  for (uint32_t i=0;i<officeHosts.GetN();++i) SetPos(officeHosts.Get(i), 85 + 8*i, 70);
  SetPos(uniGw,    90, 20);
  for (uint32_t i=0;i<uniHosts.GetN();++i)    SetPos(uniHosts.Get(i), 85 + 8*i, 10);
  SetPos(iotGw,    10, 20);
  for (uint32_t i=0;i<iotHosts.GetN();++i)    SetPos(iotHosts.Get(i), 5 + 8*i, 10);

  // Connect PGW to CORE via P2P so the rest of the city reaches UEs via CORE
  NetDeviceContainer pgwToCore = p2p.Install(NodeContainer(pgw, core));

  // NR helpers: spectrum/channel/beamforming defaults (keep it simple)
  nr->SetBeamformingHelper(CreateObject<IdealBeamformingHelper>());
  nr->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));

  // GNB + UE PHY/MAC
  NetDeviceContainer gNbDevs = nr->InstallGnbDevice(gNbNodes);
  NetDeviceContainer ueDevs  = nr->InstallUeDevice(ueNodes);

  // Attach UE to gNB
  nr->AttachToClosestEnb(ueDevs, gNbDevs);

  // IP for UE side goes via EPC (assigns 7.x/8.x etc. internally)
  epc->AssignUeIpv4Address(NetDeviceContainer(ueDevs));

  // Router interfaces
  Ipv4AddressHelper addr;
  addr.SetBase("10.1.1.0", "255.255.255.0"); Ipv4InterfaceContainer ifHomeLan   = addr.Assign(homeLanDevs);
  addr.SetBase("10.1.2.0", "255.255.255.0"); Ipv4InterfaceContainer ifOfficeLan = addr.Assign(officeLanDevs);
  addr.SetBase("10.1.3.0", "255.255.255.0"); Ipv4InterfaceContainer ifUniLan    = addr.Assign(uniLanDevs);
  addr.SetBase("10.1.4.0", "255.255.255.0"); Ipv4InterfaceContainer ifIotLan    = addr.Assign(iotLanDevs);

  // PGW <-> CORE link (/30)
  addr.SetBase("10.255.100.0", "255.255.255.252");
  Ipv4InterfaceContainer ifPgwCore = addr.Assign(pgwToCore);

  // Static/global routing
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // ---- Apps ----
  // 1) Home H1 -> University U1 (UDP Echo)
  Ptr<Node> H1 = homeHosts.Get(0);
  Ptr<Node> U1 = uniHosts.Get(0);
  uint16_t echoPort = 9;
  UdpEchoServerHelper echoServer(echoPort);
  auto echoSrvApp = echoServer.Install(U1);
  echoSrvApp.Start(Seconds(1.0)); echoSrvApp.Stop(Seconds(12.0));
  // U1 address on Uni LAN
  Ipv4Address u1Addr = U1->GetObject<Ipv4>()->GetAddress(1,0).GetLocal();
  UdpEchoClientHelper echoClient(u1Addr, echoPort);
  echoClient.SetAttribute("MaxPackets", UintegerValue(6));
  echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
  echoClient.SetAttribute("PacketSize", UintegerValue(512));
  auto echoCliApp = echoClient.Install(H1);
  echoCliApp.Start(Seconds(2.0)); echoCliApp.Stop(Seconds(12.0));

  // 2) IoT I1 -> Home H2 (UDP OnOff to a sink)
  Ptr<Node> I1 = iotHosts.Get(0);
  Ptr<Node> H2 = homeHosts.Get(1);
  uint16_t iotPort = 4000;
  Address sinkAddr (InetSocketAddress (H2->GetObject<Ipv4>()->GetAddress(1,0).GetLocal(), iotPort));
  PacketSinkHelper sinkUdp ("ns3::UdpSocketFactory", sinkAddr);
  auto sinkApp = sinkUdp.Install(H2);
  sinkApp.Start(Seconds(1.0)); sinkApp.Stop(Seconds(12.0));
  OnOffHelper onoff ("ns3::UdpSocketFactory", sinkAddr);
  onoff.SetAttribute("DataRate", StringValue("2Mbps"));
  onoff.SetAttribute("PacketSize", UintegerValue(300));
  onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
  onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
  auto iotApp = onoff.Install(I1);
  iotApp.Start(Seconds(3.0)); iotApp.Stop(Seconds(12.0));

  // 3) Office O1 -> University U2 (TCP BulkSend)
  Ptr<Node> O1 = officeHosts.Get(0);
  Ptr<Node> U2 = uniHosts.Get(1);
  uint16_t tcpPort = 5001;
  Address sinkAddrTcp (InetSocketAddress (U2->GetObject<Ipv4>()->GetAddress(1,0).GetLocal(), tcpPort));
  PacketSinkHelper sinkTcp ("ns3::TcpSocketFactory", sinkAddrTcp);
  auto sinkTcpApp = sinkTcp.Install(U2);
  sinkTcpApp.Start(Seconds(1.0)); sinkTcpApp.Stop(Seconds(12.0));
  BulkSendHelper bulk ("ns3::TcpSocketFactory", sinkAddrTcp);
  bulk.SetAttribute("MaxBytes", UintegerValue(0));
  bulk.SetAttribute("SendSize", UintegerValue(1024));
  auto bulkApp = bulk.Install(O1);
  bulkApp.Start(Seconds(4.0)); bulkApp.Stop(Seconds(12.0));

  // ---- Tracing ----
  // PCAPs (CSMA LAN taps + PGW-CORE)
  csma.EnablePcap("mkNR-home", homeLanDevs.Get(1), true);
  csma.EnablePcap("mkNR-office", officeLanDevs.Get(1), true);
  csma.EnablePcap("mkNR-uni", uniLanDevs.Get(1), true);
  csma.EnablePcap("mkNR-iot", iotLanDevs.Get(1), true);
  p2p.EnablePcapAll("mkNR-pgw-core");

  // FlowMonitor
  FlowMonitorHelper fmh; Ptr<FlowMonitor> fm = fmh.InstallAll();

  // NetAnim
  AnimationInterface anim("mk-nr-home.xml");
  anim.UpdateNodeDescription(core, "CORE");
  anim.UpdateNodeDescription(homeGw, "Home-UE");
  anim.UpdateNodeDescription(gNbNodes.Get(0), "gNB");
  anim.UpdateNodeDescription(officeGw, "Office-GW");
  anim.UpdateNodeDescription(uniGw, "Uni-GW");
  anim.UpdateNodeDescription(iotGw, "IoT-GW");
  for (uint32_t i=0;i<homeHosts.GetN();++i)  anim.UpdateNodeDescription(homeHosts.Get(i),  "H"+std::to_string(i+1));
  for (uint32_t i=0;i<officeHosts.GetN();++i) anim.UpdateNodeDescription(officeHosts.Get(i),"O"+std::to_string(i+1));
  for (uint32_t i=0;i<uniHosts.GetN();++i)    anim.UpdateNodeDescription(uniHosts.Get(i),  "U"+std::to_string(i+1));
  for (uint32_t i=0;i<iotHosts.GetN();++i)    anim.UpdateNodeDescription(iotHosts.Get(i),  "I"+std::to_string(i+1));

  Simulator::Stop(Seconds(12.0));
  Simulator::Run();
  fm->SerializeToXmlFile("mkNR-flow.xml", true, true);
  Simulator::Destroy();
  return 0;
}