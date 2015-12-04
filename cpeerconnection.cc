/**
 * C wrapper for the C++ PeerConnection code, to be go-compatible.
 */
#include "cpeerconnection.h"
#include "webrtc/base/common.h"
#include "talk/app/webrtc/peerconnectioninterface.h"
#include "talk/app/webrtc/test/fakeconstraints.h"
#include <iostream>
#include <unistd.h>
#include <future>

#define SUCCESS 0
#define FAILURE 1
#define TIMEOUT_SECS 3

using namespace std;
using namespace webrtc;

// Smaller typedefs
typedef rtc::scoped_refptr<webrtc::PeerConnectionInterface> PC;
typedef SessionDescriptionInterface* SDP;


// Peer acts as the glue between go and native code PeerConnectionInterface.
// However, it's not directly accessible from the Go side, which can only
// see what's exposed in the more pure extern "C" header file...
//
// This class also stubs libwebrtc's callback interface to be blocking,
// which allows the usage of goroutines, which is more idiomatic and easier
// for users of this library.
// The alternative would require casting Go function pointers, calling Go code
// from C code from Go code, which is less likely to be a good time.
//
// TODO(keroserene): More documentation...
class Peer
  : public CreateSessionDescriptionObserver,
    public PeerConnectionObserver {
 public:

  bool Initialize() {
    // Prepare everything.
    // Should be called before anything else happens.
    promiseSDP = promise<SDP>();
    // Due to the different threading model, in order for PeerConnectionFactory
    // to be able to post async messages without getting blocked, we need to use
    // external signalling and worker threads.
    signal_thread = new rtc::Thread();
    worker_thread = new rtc::Thread();
    signal_thread->Start();
    worker_thread->Start();
    pc_factory = CreatePeerConnectionFactory(
      signal_thread,
      worker_thread,
      NULL, NULL, NULL);
    if (!pc_factory.get()) {
      cout << "ERROR: Could not create PeerConnectionFactory" << endl;
      return false;
    }
    // PortAllocatorFactoryInterface *allocator;

    // TODO: DTLS
    // TODO: Make actual media constraints, with an exposed Go interface.
    auto c = new FakeConstraints();
    c->AddOptional(MediaConstraintsInterface::kEnableDtlsSrtp, "false");
    c->SetMandatoryReceiveAudio(false);
    c->SetMandatoryReceiveVideo(false);
    constraints = c;
    cout << "[C] Peer initialized." << endl;
    return true;
  }

  void resetPromise() {
    // delete &promiseSDP;
    promiseSDP = promise<SDP>();
  }

  //
  // CreateSessionDescriptionObserver implementation
  //
  void OnSuccess(SDP desc) {
    cout << "[C] SDP successfully created at " << desc << endl;
    promiseSDP.set_value(desc);
  }

  void OnFailure(const std::string& error) {
    cout << "failure: " << error << endl;
    promiseSDP.set_value(NULL);
  }

  //
  // PeerConnectionObserver Implementation
  // TODO: cgo hooks
  //
  void OnStateChange(PeerConnectionObserver::StateType state) {
    cout << "OnStateChange" << endl;
  }

  void OnAddStream(webrtc::MediaStreamInterface* stream) {
    cout << "OnAddStream" << endl;
  }

  void OnRemoveStream(webrtc::MediaStreamInterface* stream) {
    cout << "OnRemoveStream" << endl;
  }

  void OnRenegotiationNeeded() {
    cout << "OnRenegotiationNeeded" << endl;
  }

  void OnIceCandidate(const IceCandidateInterface* candidate) {
    cout << "OnIceCandidate" << candidate << endl;
  }

  void OnDataChannel(DataChannelInterface* data_channel) {
    cout << "OnDataChannel" << endl;
  }

  PeerConnectionInterface::RTCConfiguration *config;
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  const MediaConstraintsInterface* constraints;

  PC pc_;

  // Passing SDPs through promises instead of callbacks, to allow the benefits
  // as described above.
  // However, this has the effect that CreateOffer and CreateAnswer must not be
  // concurrent, to themselves or each other (which isn't expected anyways) due
  // to the simplistic way in which futures are used here.
  promise<SDP> promiseSDP;

  rtc::scoped_refptr<PeerConnectionFactoryInterface> pc_factory;
  // TODO: prepare and expose IceServers for real.
  PeerConnectionInterface::IceServers ice_servers;

 protected:
  rtc::Thread *signal_thread;
  rtc::Thread *worker_thread;

};  // class Peer

// Global variable to prevent deallocatunio, due to the required scoped_refptr.
// (Probably fix later)
rtc::scoped_refptr<Peer> localPeer;


// TODO: Make a better generalized class for every "Observer" later.
class PeerSDPObserver : public SetSessionDescriptionObserver {
 public:
  static PeerSDPObserver* Create() {
    return new rtc::RefCountedObject<PeerSDPObserver>();
  }
  virtual void OnSuccess() {
    cout << "local description set!" << endl;
  }
  virtual void OnFailure(const std::string& error) {
    cout << error << endl;
  }

 protected:
  PeerSDPObserver() {}
  ~PeerSDPObserver() {}

};  // class PeerSDPObserver


//
// extern "C" Go-accessible functions.
//

// Create and return the Peer object, which provides initial native code
// glue for the PeerConnection constructor.
CGOPeer CGOInitializePeer() {
  localPeer = new rtc::RefCountedObject<Peer>();
  localPeer->Initialize();
  return localPeer;
}


// Create and return a PeerConnection object.
// This cannot be a method in |Peer|, because this must be accessible to cgo.
CGOPeer NewPeerConnection(CGOPeer cgoPeer) {
  Peer *peer = (Peer*)cgoPeer;
  PeerConnectionInterface::IceServer *server = new
      PeerConnectionInterface::IceServer();
  server->uri = "stun:stun.l.google.com:19302";
  peer->ice_servers.push_back(*server);

  // Prepare RTC Configuration object. This is just the default one, for now.
  // TODO: A Go struct that can be passed and converted here.
  peer->config = new PeerConnectionInterface::RTCConfiguration();
  peer->config->servers = peer->ice_servers;

  // Prepare a native PeerConnection object.
  peer->pc_ = peer->pc_factory->CreatePeerConnection(
    *peer->config,
    peer->constraints,
    NULL,  // port allocator
    NULL,  // TODO: DTLS
    peer
    );
  if (!peer->pc_.get()) {
    cout << "ERROR: Could not create PeerConnection." << endl;
    return NULL;
  }
  cout << "[C] Made a PeerConnection: " << peer->pc_ << endl;
  return (CGOPeer)peer;
}

bool SDPtimeout(future<SDP> *f, int seconds) {
  auto status = f->wait_for(chrono::seconds(TIMEOUT_SECS));
  return future_status::ready != status;
}

// PeerConnection::CreateOffer
// Blocks until libwebrtc succeeds in generating the SDP offer,
// @returns SDP (pointer), or NULL on timeeout.
CGOsdp CGOCreateOffer(CGOPeer cgoPeer) {
  // TODO: Provide an actual RTCOfferOptions as an argument.
  Peer* peer = (Peer*)cgoPeer;
  auto r = peer->promiseSDP.get_future();
  peer->pc_->CreateOffer(peer, peer->constraints);
  if (SDPtimeout(&r, TIMEOUT_SECS)) {
    cout << "[C] CreateOffer timed out after " << TIMEOUT_SECS << endl;
    peer->resetPromise();
    return NULL;
  }
  SDP sdp = r.get();  // blocking
  peer->resetPromise();
  return (CGOsdp)sdp;
}


// PeerConnection::CreateAnswer
// Blocks until libwebrtc succeeds in generating the SDP answer.
// @returns SDP, or NULL on timeout.
CGOsdp CGOCreateAnswer(CGOPeer cgoPeer) {
  Peer *peer = (Peer*)cgoPeer;
  cout << "[C] CreateAnswer" << peer << endl;
  auto r = peer->promiseSDP.get_future();
  peer->pc_->CreateAnswer(peer, peer->constraints);
  if (SDPtimeout(&r, TIMEOUT_SECS)) {
    cout << "[C] CreateAnswer timed out after " << TIMEOUT_SECS << endl;
    peer->resetPromise();
    return NULL;
  }
  SDP sdp = r.get();  // blocking
  peer->resetPromise();
  return (CGOsdp)sdp;
}


// Serialize SDP message to a string Go can use.
CGOsdpString CGOSerializeSDP(CGOsdp sdp) {
  auto s = new string();
  SDP cSDP = (SDP)sdp;
  cSDP->ToString(s);
  return (CGOsdpString)s->c_str();
}


void CGOSetLocalDescription(CGOPeer pc, CGOsdp sdp) {
  PC cPC = ((Peer*)pc)->pc_;
  cPC->SetLocalDescription(PeerSDPObserver::Create(), (SDP)sdp);
}
