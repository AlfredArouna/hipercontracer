// =================================================================
//          #     #                 #     #
//          ##    #   ####   #####  ##    #  ######   #####
//          # #   #  #    #  #    # # #   #  #          #
//          #  #  #  #    #  #    # #  #  #  #####      #
//          #   # #  #    #  #####  #   # #  #          #
//          #    ##  #    #  #   #  #    ##  #          #
//          #     #   ####   #    # #     #  ######     #
//
//       ---   The NorNet Testbed for Multi-Homed Systems  ---
//                       https://www.nntb.no
// =================================================================
//
// High-Performance Connectivity Tracer (HiPerConTracer)
// Copyright (C) 2015-2019 by Thomas Dreibholz
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// Contact: dreibh@simula.no

#include "traceroute.h"
#include "tools.h"
#include "logger.h"
#include "icmpheader.h"
#include "ipv4header.h"
#include "ipv6header.h"
#include "traceserviceheader.h"

#include <netinet/in.h>
#include <netinet/ip.h>

#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/version.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>
#if BOOST_VERSION >= 106600
#include <boost/uuid/detail/sha1.hpp>
#else
#include <boost/uuid/sha1.hpp>
#endif


// ###### Constructor #######################################################
ResultEntry::ResultEntry(const unsigned short                        round,
                         const unsigned short                        seqNumber,
                         const unsigned int                          hop,
                         const uint16_t                              checksum,
                         const std::chrono::system_clock::time_point sendTime,
                         const boost::asio::ip::address&             address,
                         const HopStatus                             status)
   : Round(round),
     SeqNumber(seqNumber),
     Hop(hop),
     Checksum(checksum),
     SendTime(sendTime),
     Address(address),
     Status(status)
{
}


// ###### Output operator ###################################################
std::ostream& operator<<(std::ostream& os, const ResultEntry& resultEntry)
{
   os << boost::format("R%d")             % resultEntry.Round
      << "\t" << boost::format("#%05d")   % resultEntry.SeqNumber
      << "\t" << boost::format("%2d")     % resultEntry.Hop
      << "\t" << boost::format("%9.3fms") % (std::chrono::duration_cast<std::chrono::microseconds>(resultEntry.rtt()).count() / 1000.0)
      << "\t" << boost::format("%3d")     % resultEntry.Status
      << "\t" << boost::format("%04x")    % resultEntry.Checksum
      << "\t" << resultEntry.Address;
   return(os);
}


// ###### Constructor #######################################################
Traceroute::Traceroute(ResultsWriter*                            resultsWriter,
                       const unsigned int                        iterations,
                       const bool                                removeDestinationAfterRun,
                       const boost::asio::ip::address&           sourceAddress,
                       const std::set<boost::asio::ip::address>& destinationAddressArray,
                       const unsigned long long                  interval,
                       const unsigned int                        expiration,
                       const unsigned int                        rounds,
                       const unsigned int                        initialMaxTTL,
                       const unsigned int                        finalMaxTTL,
                       const unsigned int                        incrementMaxTTL)
   : ResultsOutput(resultsWriter),
     Iterations(iterations),
     RemoveDestinationAfterRun(removeDestinationAfterRun),
     Interval(interval),
     Expiration(expiration),
     Rounds(rounds),
     InitialMaxTTL(initialMaxTTL),
     FinalMaxTTL(finalMaxTTL),
     IncrementMaxTTL(incrementMaxTTL),
     IOService(),
     SourceAddress(sourceAddress),
     ICMPSocket(IOService, (isIPv6() == true) ? boost::asio::ip::icmp::v6() : boost::asio::ip::icmp::v4()),
     TimeoutTimer(IOService),
     IntervalTimer(IOService)
{
   // ====== Some initialisations ===========================================
   Identifier          = 0;
   SeqNumber           = (unsigned short)(std::rand() & 0xffff);
   MagicNumber         = ((std::rand() & 0xffff) << 16) | (std::rand() & 0xffff);
   OutstandingRequests = 0;
   LastHop             = 0xffffffff;
   ExpectingReply      = false;
   StopRequested       = false;
   IterationNumber     = 0;
   MinTTL              = 1;
   MaxTTL              = InitialMaxTTL;
   TargetChecksumArray = new uint32_t[Rounds];
   assert(TargetChecksumArray != NULL);

   // ====== Prepare destination endpoints ==================================
   std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);
   for(std::set<boost::asio::ip::address>::const_iterator destinationAddressIterator = destinationAddressArray.begin();
       destinationAddressIterator != destinationAddressArray.end(); destinationAddressIterator++) {
      const boost::asio::ip::address destinationAddress = *destinationAddressIterator;
      if(destinationAddress.is_v6() == SourceAddress.is_v6()) {
         DestinationAddresses.insert(destinationAddress);
      }
   }
   DestinationAddressIterator = DestinationAddresses.end();
}


// ###### Get source address ################################################
const boost::asio::ip::address& Traceroute::getSource()
{
    return(SourceAddress);
}


// ###### Add destination address ###########################################
bool Traceroute::addDestination(const boost::asio::ip::address& destinationAddress)
{
   if(destinationAddress.is_v6() == SourceAddress.is_v6()) {
      std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);
      const std::set<boost::asio::ip::address>::const_iterator destinationIterator =
         DestinationAddresses.find(destinationAddress);

      if(destinationIterator == DestinationAddresses.end()) {
         if(DestinationAddressIterator == DestinationAddresses.end()) {
            // Address will be the first destination in list -> abort interval timer
            IntervalTimer.expires_from_now(boost::posix_time::milliseconds(0));
            IntervalTimer.async_wait(boost::bind(&Traceroute::handleIntervalEvent, this,
                                                 boost::asio::placeholders::error));
         }
         DestinationAddresses.insert(destinationAddress);
         return true;
      }

      // Already there -> nothing to do.
   }
   return false;
}


// ###### Destructor ########################################################
Traceroute::~Traceroute()
{
   delete [] TargetChecksumArray;
   TargetChecksumArray = NULL;
}


// ###### Start thread ######################################################
bool Traceroute::start()
{
   StopRequested = false;
   Thread        = std::thread(&Traceroute::run, this);
   return(prepareSocket());
}


// ###### Request stop of thread ############################################
void Traceroute::requestStop() {
   StopRequested = true;
   IntervalTimer.get_io_service().post(boost::bind(&Traceroute::cancelIntervalTimer, this));
   TimeoutTimer.get_io_service().post(boost::bind(&Traceroute::cancelTimeoutTimer, this));
   ICMPSocket.get_io_service().post(boost::bind(&Traceroute::cancelSocket, this));
}


// ###### Join thread #######################################################
void Traceroute::join()
{
   requestStop();
   Thread.join();
   StopRequested = false;
}


// ###### Is thread joinable? ###############################################
bool Traceroute::joinable()
{
   // Joinable, if stop is requested *and* the thread is joinable!
   return(StopRequested && Thread.joinable());
}


// ###### Prepare ICMP socket ###############################################
bool Traceroute::prepareSocket()
{
   // ====== Bind ICMP socket to given source address =======================
   boost::system::error_code errorCode;
   ICMPSocket.bind(boost::asio::ip::icmp::endpoint(SourceAddress, 0), errorCode);
   if(errorCode !=  boost::system::errc::success) {
      HPCT_LOG(error) << "Unable to bind ICMP socket to source address "
                      << SourceAddress << "!";
      return(false);
   }

   // ====== Set filter (not required, but much more efficient) =============
   if(isIPv6()) {
      struct icmp6_filter filter;
      ICMP6_FILTER_SETBLOCKALL(&filter);
      ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
      ICMP6_FILTER_SETPASS(ICMP6_DST_UNREACH, &filter);
      ICMP6_FILTER_SETPASS(ICMP6_PACKET_TOO_BIG, &filter);
      ICMP6_FILTER_SETPASS(ICMP6_TIME_EXCEEDED, &filter);
      if(setsockopt(ICMPSocket.native_handle(), IPPROTO_ICMPV6, ICMP6_FILTER,
                    &filter, sizeof(struct icmp6_filter)) < 0) {
         HPCT_LOG(warning) << "Unable to set ICMP6_FILTER!";
      }
   }
   return(true);
}


// ###### Cancel socket operations ##########################################
void Traceroute::cancelSocket()
{
   ICMPSocket.cancel();
}


// ###### Prepare a new run #################################################
bool Traceroute::prepareRun(const bool newRound)
{
   std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);

   if(newRound) {
      IterationNumber++;

      // ====== Rewind ======================================================
      DestinationAddressIterator = DestinationAddresses.begin();
      for(unsigned int i = 0; i < Rounds; i++) {
         TargetChecksumArray[i] = ~0U;   // Use a new target checksum!
      }
   }
   else {
      // ====== Get next destination address ===================================
      if(DestinationAddressIterator != DestinationAddresses.end()) {
         if(RemoveDestinationAfterRun == false) {
            DestinationAddressIterator++;
         }
         else {
            std::set<boost::asio::ip::address>::iterator toBeDeleted = DestinationAddressIterator;
            DestinationAddressIterator++;
            HPCT_LOG(debug) << "Removing " << *toBeDeleted;
            DestinationAddresses.erase(toBeDeleted);
         }
      }
   }

   // ====== Clear results ==================================================
   ResultsMap.clear();
   MinTTL              = 1;
   MaxTTL              = (DestinationAddressIterator != DestinationAddresses.end()) ?
                            getInitialMaxTTL(*DestinationAddressIterator) : InitialMaxTTL;
   LastHop             = 0xffffffff;
   OutstandingRequests = 0;
   RunStartTimeStamp   = std::chrono::steady_clock::now();

   // Return whether end of the list is reached. Then, a rewind is necessary.
   return(DestinationAddressIterator == DestinationAddresses.end());
}


// ###### Send requests to all destinations #################################
void Traceroute::sendRequests()
{
   std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);

   // ====== Send requests, if there are destination addresses ==============
   if(DestinationAddressIterator != DestinationAddresses.end()) {
      const boost::asio::ip::address& destinationAddress = *DestinationAddressIterator;
      HPCT_LOG(debug) << "Traceroute from " << SourceAddress
                      << " to " << destinationAddress << " ...";

      // ====== Send Echo Requests ==========================================
      assert(MinTTL > 0);
      for(unsigned int round = 0; round < Rounds; round++) {
         for(int ttl = (int)MaxTTL; ttl >= (int)MinTTL; ttl--) {
            sendICMPRequest(destinationAddress, (unsigned int)ttl, round,
                            TargetChecksumArray[round]);
         }
      }
      scheduleTimeoutEvent();
   }

   // ====== No destination addresses -> wait ===============================
   else {
      scheduleIntervalEvent();
   }
}


// ###### Schedule timeout timer ############################################
void Traceroute::scheduleTimeoutEvent()
{
   const unsigned int deviation = std::max(10U, Expiration / 5);   // 20% deviation
   const unsigned int duration  = Expiration + (std::rand() % deviation);
   TimeoutTimer.expires_from_now(boost::posix_time::milliseconds(duration));
   TimeoutTimer.async_wait(boost::bind(&Traceroute::handleTimeoutEvent, this,
                                       boost::asio::placeholders::error));
}


// ###### Cancel timeout timer ##############################################
void Traceroute::cancelTimeoutTimer()
{
   TimeoutTimer.cancel();
}


// ###### Schedule interval timer ###########################################
void Traceroute::scheduleIntervalEvent()
{
   if((Iterations == 0) || (IterationNumber < Iterations)) {
      // ====== Schedule event ==============================================
      const unsigned long long deviation       = std::max(10ULL, Interval / 5);   // 20% deviation
      const unsigned long long waitingDuration = Interval + (std::rand() % deviation);

      const std::chrono::steady_clock::duration howLongToWait =
         (RunStartTimeStamp + std::chrono::milliseconds(waitingDuration)) - std::chrono::steady_clock::now();
      const long long millisecondsToWait =
         std::max(0LL, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(howLongToWait).count());

      IntervalTimer.expires_from_now(boost::posix_time::milliseconds(millisecondsToWait));
      IntervalTimer.async_wait(boost::bind(&Traceroute::handleIntervalEvent, this,
                                           boost::asio::placeholders::error));
      HPCT_LOG(debug) << "Waiting " << millisecondsToWait / 1000.0
                      << "s before iteration " << (IterationNumber + 1) << " ...";

      // ====== Check, whether it is time for starting a new transaction ====
      if(ResultsOutput) {
         ResultsOutput->mayStartNewTransaction();
      }
   }
   else {
       // ====== Done -> exit! ==============================================
       StopRequested = true;
       cancelIntervalTimer();
       cancelTimeoutTimer();
       cancelSocket();
   }
}


// ###### Cancel interval timer #############################################
void Traceroute::cancelIntervalTimer()
{
   IntervalTimer.cancel();
}


// ###### Expect next ICMP message ##########################################
void Traceroute::expectNextReply()
{
   assert(ExpectingReply == false);
   ICMPSocket.async_receive_from(boost::asio::buffer(MessageBuffer),
                                 ReplyEndpoint,
                                 boost::bind(&Traceroute::handleMessage, this,
                                             boost::asio::placeholders::error,
                                             boost::asio::placeholders::bytes_transferred));
   ExpectingReply = true;
}


// ###### All requests have received a response #############################
void Traceroute::noMoreOutstandingRequests()
{
   HPCT_LOG(trace) << "Completed!";
   cancelTimeoutTimer();
}


// ###### Get value for initial MaxTTL ######################################
unsigned int Traceroute::getInitialMaxTTL(const boost::asio::ip::address& destinationAddress) const
{
   const std::map<boost::asio::ip::address, unsigned int>::const_iterator found = TTLCache.find(destinationAddress);
   if(found != TTLCache.end()) {
      return(std::min(found->second, FinalMaxTTL));
   }
   return(InitialMaxTTL);
}


// ###### Send one ICMP request to given destination ########################
void Traceroute::sendICMPRequest(const boost::asio::ip::address& destinationAddress,
                                 const unsigned int              ttl,
                                 const unsigned int              round,
                                 uint32_t&                       targetChecksum)
{
   // ====== Set TTL ========================================
   const boost::asio::ip::unicast::hops option(ttl);
   ICMPSocket.set_option(option);

   // ====== Create an ICMP header for an echo request ======
   SeqNumber++;
   ICMPHeader echoRequest;
   echoRequest.type((isIPv6() == true) ? ICMPHeader::IPv6EchoRequest : ICMPHeader::IPv4EchoRequest);
   echoRequest.code(0);
   echoRequest.identifier(Identifier);
   echoRequest.seqNumber(SeqNumber);
   TraceServiceHeader tsHeader;
   tsHeader.magicNumber(MagicNumber);
   tsHeader.sendTTL(ttl);
   tsHeader.round((unsigned char)round);
   tsHeader.checksumTweak(0);
   const std::chrono::system_clock::time_point sendTime = std::chrono::system_clock::now();
   tsHeader.sendTimeStamp(makePacketTimeStamp(sendTime));
   std::vector<unsigned char> tsHeaderContents = tsHeader.contents();

   // ====== Tweak checksum ===============================
   // ------ No given target checksum ---------------------
   if(targetChecksum == ~0U) {
      computeInternet16(echoRequest, tsHeaderContents.begin(), tsHeaderContents.end());
      targetChecksum = echoRequest.checksum();
   }
   // ------ Target checksum given ------------------------
   else {
      // Compute current checksum
      computeInternet16(echoRequest, tsHeaderContents.begin(), tsHeaderContents.end());
      const uint16_t originalChecksum = echoRequest.checksum();

      // Compute value to tweak checksum to target value
      uint16_t diff = 0xffff - (targetChecksum - originalChecksum);
      if(originalChecksum > targetChecksum) {    // Handle necessary sum wrap!
         diff++;
      }
      tsHeader.checksumTweak(diff);

      // Compute new checksum (must be equal to target checksum!)
      tsHeaderContents = tsHeader.contents();
      computeInternet16(echoRequest, tsHeaderContents.begin(), tsHeaderContents.end());
      assert(echoRequest.checksum() == targetChecksum);
   }

   // ====== Encode the request packet ======================
   boost::asio::streambuf request_buffer;
   std::ostream os(&request_buffer);
   os << echoRequest << tsHeader;

   // ====== Send the request ===============================
   std::size_t sent;
   try {
      sent = ICMPSocket.send_to(request_buffer.data(), boost::asio::ip::icmp::endpoint(destinationAddress, 0));
   }
   catch (boost::system::system_error const& e) {
      sent = -1;
   }
   if(sent < 1) {
      HPCT_LOG(warning) << "Traceroute::sendICMPRequest() - ICMP send_to("
                        << SourceAddress << "->" << destinationAddress << ") failed!";
   }
   else {
      // ====== Record the request ==========================
      OutstandingRequests++;

      assert((targetChecksum & ~0xffff) == 0);
      ResultEntry resultEntry(round, SeqNumber, ttl, (uint16_t)targetChecksum, sendTime,
                              destinationAddress, Unknown);
      std::pair<std::map<unsigned short, ResultEntry>::iterator, bool> result = ResultsMap.insert(std::pair<unsigned short, ResultEntry>(SeqNumber,resultEntry));
      assert(result.second == true);
   }
}


// ###### Run the measurement ###############################################
void Traceroute::run()
{
   Identifier = ::getpid();   // Identifier is the process ID
   // NOTE: Assuming 16-bit PID, and one PID per thread!

   prepareRun(true);
   sendRequests();
   expectNextReply();

   IOService.run();
}


// ###### The destination has not been reached with the current TTL #########
bool Traceroute::notReachedWithCurrentTTL()
{
   std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);

   if(MaxTTL < FinalMaxTTL) {
      MinTTL = MaxTTL + 1;
      MaxTTL = std::min(MaxTTL + IncrementMaxTTL, FinalMaxTTL);
      HPCT_LOG(debug) << "Cannot reach " << *DestinationAddressIterator
                      << " with TTL " << MinTTL - 1 << ", now trying TTLs "
                      << MinTTL << " to " << MaxTTL << " ...";
      return(true);
   }
   return(false);
}


// ###### Comparison function for results output ############################
int Traceroute::compareTracerouteResults(const ResultEntry* a, const ResultEntry* b)
{
  return(a->hop() < b->hop());
}


// ###### Process results ###################################################
void Traceroute::processResults()
{
   uint64_t timeStamp = 0;

   // ====== Sort results ===================================================
   std::vector<ResultEntry*> resultsVector;
   for(std::map<unsigned short, ResultEntry>::iterator iterator = ResultsMap.begin(); iterator != ResultsMap.end(); iterator++) {
      resultsVector.push_back(&iterator->second);
   }
   std::sort(resultsVector.begin(), resultsVector.end(), &compareTracerouteResults);

   // ====== Handle the results of each round ===============================
   for(unsigned int round = 0; round < Rounds; round++) {

      // ====== Count hops ==================================================
      std::size_t totalHops          = 0;
      std::size_t currentHop         = 0;
      bool        completeTraceroute = true;   // all hops have responded
      bool        destinationReached = false;  // destination has responded
      std::string pathString         = SourceAddress.to_string();
      for(std::vector<ResultEntry*>::iterator iterator = resultsVector.begin(); iterator != resultsVector.end(); iterator++) {
         ResultEntry* resultEntry = *iterator;
         if(resultEntry->round() == round) {
            assert(resultEntry->hop() > totalHops);
            currentHop++;
            totalHops = resultEntry->hop();

            // ====== We have reached the destination =======================
            if(resultEntry->status() == Success) {
               pathString += "-" + resultEntry->address().to_string();
               destinationReached = true;
               break;   // done!
            }

            // ====== Unreachable (as reported by router) ===================
            else if(statusIsUnreachable(resultEntry->status())) {
               pathString += "-" + resultEntry->address().to_string();
               break;   // we can stop here!
            }

            // ====== Time-out ==============================================
            else if(resultEntry->status() == Unknown) {
               resultEntry->status(Timeout);
               resultEntry->receiveTime(resultEntry->sendTime() + std::chrono::milliseconds(Expiration));
               pathString += "-*";
               completeTraceroute = false;   // at least one hop has not sent a response :-(
            }

            // ====== Some other response (usually TTL exceeded) ============
            else {
               pathString += "-" + resultEntry->address().to_string();
            }
         }
      }
      assert(currentHop == totalHops);

      // ====== Compute path hash ===========================================
      // Checksum: the first 64 bits of the SHA-1 sum over path string
      boost::uuids::detail::sha1 sha1Hash;
      sha1Hash.process_bytes(pathString.c_str(), pathString.length());
      uint32_t digest[5];
      sha1Hash.get_digest(digest);
      const uint64_t pathHash    = ((uint64_t)digest[0] << 32) | (uint64_t)digest[1];
      unsigned int   statusFlags = 0x0000;
      if(!completeTraceroute) {
         statusFlags |= Flag_StarredRoute;
      }
      if(destinationReached) {
         statusFlags |= Flag_DestinationReached;
      }

      // ====== Print traceroute entries =======================================
      HPCT_LOG(debug) << "Round " << round << ":";

      bool     writeHeader   = true;
      uint16_t checksumCheck = 0;
      for(std::vector<ResultEntry*>::iterator iterator = resultsVector.begin(); iterator != resultsVector.end(); iterator++) {
         ResultEntry* resultEntry = *iterator;
         if(resultEntry->round() == round) {
            HPCT_LOG(debug) << *resultEntry;

            if(ResultsOutput) {
               if(timeStamp == 0) {
                  // Time stamp for this traceroute run is the first entry's send time!
                  // => Necessary, in order to ensure that all entries have the same time stamp.
                  timeStamp = usSinceEpoch(resultEntry->sendTime());
               }

               if(writeHeader) {
                  ResultsOutput->insert(
                     str(boost::format("#T %s %s %x %d %x %d %x %x")
                        % SourceAddress.to_string()
                        % (*DestinationAddressIterator).to_string()
                        % timeStamp
                        % round
                        % resultEntry->checksum()
                        % totalHops
                        % statusFlags
                        % (int64_t)pathHash
                  ));
                  writeHeader = false;
                  checksumCheck = resultEntry->checksum();
               }

               ResultsOutput->insert(
                  str(boost::format("\t %d %x %d %s")
                     % resultEntry->hop()
                     % (unsigned int)resultEntry->status()
                     % std::chrono::duration_cast<std::chrono::microseconds>(resultEntry->receiveTime() - resultEntry->sendTime()).count()
                     % resultEntry->address().to_string()
               ));
               assert(resultEntry->checksum() == checksumCheck);
            }

            if( (resultEntry->status() == Success) ||
                (statusIsUnreachable(resultEntry->status())) ) {
               break;
            }
         }
      }
   }
}


// ###### Handle timer event ################################################
void Traceroute::handleTimeoutEvent(const boost::system::error_code& errorCode)
{
   std::lock_guard<std::recursive_mutex> lock(DestinationAddressMutex);

   // ====== Stop requested? ================================================
   if(StopRequested) {
      return;
   }

   // ====== Has destination been reached with current TTL? =================
   if(DestinationAddressIterator != DestinationAddresses.end()) {
      TTLCache[*DestinationAddressIterator] = LastHop;
      if(LastHop == 0xffffffff) {
         if(notReachedWithCurrentTTL()) {
            // Try another round ...
            sendRequests();
            return;
         }
      }
   }

   // ====== Create results output ==========================================
   processResults();

   // ====== Prepare new run ================================================
   if(prepareRun() == false) {
      sendRequests();
   }
   else {
      // Done with this round -> schedule next round!
      scheduleIntervalEvent();
   }
}


// ###### Handle timer event ################################################
void Traceroute::handleIntervalEvent(const boost::system::error_code& errorCode)
{
   // ====== Stop requested? ================================================
   if(StopRequested) {
      return;
   }

   // ====== Prepare new run ================================================
   if(errorCode != boost::asio::error::operation_aborted) {
      HPCT_LOG(debug) << "Starting iteration " << (IterationNumber + 1) << " ...";
      prepareRun(true);
      sendRequests();
   }
}


// ###### Handle incoming ICMP message ######################################
void Traceroute::handleMessage(const boost::system::error_code& errorCode,
                               std::size_t                      length)
{
   if(errorCode != boost::asio::error::operation_aborted) {
      if(!errorCode) {
         const std::chrono::system_clock::time_point receiveTime = std::chrono::system_clock::now();
         boost::interprocess::bufferstream is(MessageBuffer, length);
         ExpectingReply = false;   // Need to call expectNextReply() to get next message!

         ICMPHeader icmpHeader;
         if(isIPv6()) {
            is >> icmpHeader;
            if(is) {
               if(icmpHeader.type() == ICMPHeader::IPv6EchoReply) {
                  if(icmpHeader.identifier() == Identifier) {
                     TraceServiceHeader tsHeader;
                     is >> tsHeader;
                     if(is) {
                        if(tsHeader.magicNumber() == MagicNumber) {
                           recordResult(receiveTime, icmpHeader, icmpHeader.seqNumber());
                        }
                     }
                  }
               }
               else if( (icmpHeader.type() == ICMPHeader::IPv6TimeExceeded) ||
                        (icmpHeader.type() == ICMPHeader::IPv6Unreachable) ) {
                  IPv6Header innerIPv6Header;
                  ICMPHeader innerICMPHeader;
                  TraceServiceHeader tsHeader;
                  is >> innerIPv6Header >> innerICMPHeader >> tsHeader;
                  if(is) {
                     if(tsHeader.magicNumber() == MagicNumber) {
                        recordResult(receiveTime, icmpHeader, innerICMPHeader.seqNumber());
                     }
                  }
               }
            }
         }
         else {
            IPv4Header ipv4Header;
            is >> ipv4Header;
            if(is) {
               is >> icmpHeader;
               if(is) {
                  if(icmpHeader.type() == ICMPHeader::IPv4EchoReply) {
                     if(icmpHeader.identifier() == Identifier) {
                        TraceServiceHeader tsHeader;
                        is >> tsHeader;
                        if(is) {
                           if(tsHeader.magicNumber() == MagicNumber) {
                              recordResult(receiveTime, icmpHeader, icmpHeader.seqNumber());
                           }
                        }
                     }
                  }
                  else if(icmpHeader.type() == ICMPHeader::IPv4TimeExceeded) {
                     IPv4Header innerIPv4Header;
                     ICMPHeader innerICMPHeader;
                     is >> innerIPv4Header >> innerICMPHeader;
                     if(is) {
                        if( (icmpHeader.type() == ICMPHeader::IPv4TimeExceeded) ||
                            (icmpHeader.type() == ICMPHeader::IPv4Unreachable) ) {
                           if(innerICMPHeader.identifier() == Identifier) {
                              // Unfortunately, ICMPv4 does not return the full TraceServiceHeader here!
                              recordResult(receiveTime, icmpHeader, innerICMPHeader.seqNumber());
                           }
                        }
                     }
                  }
               }
            }
         }
      }

      if(OutstandingRequests == 0) {
         noMoreOutstandingRequests();
      }
      expectNextReply();
   }
}


// ###### Record result from response message ###############################
void Traceroute::recordResult(const std::chrono::system_clock::time_point& receiveTime,
                              const ICMPHeader&               icmpHeader,
                              const unsigned short            seqNumber)
{
   // ====== Find corresponding request =====================================
   std::map<unsigned short, ResultEntry>::iterator found = ResultsMap.find(seqNumber);
   if(found == ResultsMap.end()) {
      return;
   }
   ResultEntry& resultEntry = found->second;

   // ====== Get status =====================================================
   if(resultEntry.status() == Unknown) {
      resultEntry.receiveTime(receiveTime);
      resultEntry.address(ReplyEndpoint.address());

      HopStatus status = Unknown;
      if( (icmpHeader.type() == ICMPHeader::IPv6TimeExceeded) ||
          (icmpHeader.type() == ICMPHeader::IPv4TimeExceeded) ) {
         status = TimeExceeded;
      }
      else if( (icmpHeader.type() == ICMPHeader::IPv6Unreachable) ||
               (icmpHeader.type() == ICMPHeader::IPv4Unreachable) ) {
         if(isIPv6()) {
            switch(icmpHeader.code()) {
               case ICMP6_DST_UNREACH_ADMIN:
                  status = UnreachableProhibited;
               break;
               case ICMP6_DST_UNREACH_BEYONDSCOPE:
                  status = UnreachableScope;
               break;
               case ICMP6_DST_UNREACH_NOROUTE:
                  status = UnreachableNetwork;
               break;
               case ICMP6_DST_UNREACH_ADDR:
                  status = UnreachableHost;
               break;
               case ICMP6_DST_UNREACH_NOPORT:
                  status = UnreachablePort;
               break;
               default:
                  status = UnreachableUnknown;
               break;
            }
         }
         else {
            switch(icmpHeader.code()) {
               case ICMP_UNREACH_FILTER_PROHIB:
                  status = UnreachableProhibited;
               break;
               case ICMP_UNREACH_NET:
               case ICMP_UNREACH_NET_UNKNOWN:
                  status = UnreachableNetwork;
               break;
               case ICMP_UNREACH_HOST:
               case ICMP_UNREACH_HOST_UNKNOWN:
                  status = UnreachableHost;
               break;
               case ICMP_UNREACH_PORT:
                  status = UnreachablePort;
               break;
               default:
                  status = UnreachableUnknown;
               break;
            }
         }
      }
      else if( (icmpHeader.type() == ICMPHeader::IPv6EchoReply) ||
               (icmpHeader.type() == ICMPHeader::IPv4EchoReply) ) {
         status  = Success;
         LastHop = std::min(LastHop, resultEntry.hop());
      }
      resultEntry.status(status);
      if(OutstandingRequests > 0) {
         OutstandingRequests--;
      }
   }
}


// ###### Convert ptime to microseconds #####################################
unsigned long long Traceroute::makePacketTimeStamp(const std::chrono::system_clock::time_point& time)
{
   // For HiPerConTracer packets: time stamp is microseconds since 1976-09-26.
   static const std::chrono::system_clock::time_point epoch =
      std::chrono::system_clock::from_time_t(212803200);
   return std::chrono::duration_cast<std::chrono::microseconds>(time - epoch).count();
}
