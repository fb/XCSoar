/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000-2016 The XCSoar Project
  A detailed list of copyright holders can be found in the file "AUTHORS".

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}
*/
#include "SkysightAPI.hpp"
#include "APIQueue.hpp"
#include "APIGlue.hpp"
#include "Request.hpp"
#include "CDFDecoder.hpp"
#include "Metrics.hpp"
#include "Event/Timer.hpp"
#include "Time/BrokenDateTime.hpp"
#include <vector>



SkysightAPIQueue::~SkysightAPIQueue() { 
  Timer::Cancel();
}

void SkysightAPIQueue::AddRequest(std::unique_ptr<SkysightAsyncRequest> &&request, bool append_end) {

  request_queue.push(std::move(request));
  if(!append_end) {
    //Login requests jump to the front of the queue
    request_queue.front().swap(request_queue.back());
  }
  if(!is_busy)
    Process();
}

void SkysightAPIQueue::AddDecodeJob(std::unique_ptr<CDFDecoder> &&job) {
  decode_queue.push(std::move(job));
  if(!is_busy)
    Process();
}
  

void SkysightAPIQueue::Process() {
  is_busy = true;
  
  if(!request_queue.empty()) {
    std::unique_ptr<SkysightAsyncRequest> job = std::move(request_queue.front());
    switch(job->GetStatus()) {
      case SkysightRequest::Status::Idle:
        
        //Provide the job with the very latest API key just prior to execution
        if(job->GetType() == SkysightCallType::Login) {
          job->SetCredentials("XCSoar", email.c_str(), password.c_str());
          job->Process();
        } else {
          if (!IsLoggedIn()) {
            // inject a login request at the front of the queue
            SkysightAPI::GenerateLoginRequest();
          } else {
            job->SetCredentials(key.c_str());
            job->Process();
          }
        }

        if(!Timer::IsActive())
          Timer::Schedule(std::chrono::milliseconds(300));
        break;
      case SkysightRequest::Status::Complete:
      case SkysightRequest::Status::Error:
        job->Done();
        break;
      case SkysightRequest::Status::Busy:
        break;
    }
  }
  
  if(!decode_queue.empty()) {
    std::unique_ptr<CDFDecoder> decode_job = std::move(decode_queue.front());
     switch(decode_job->GetStatus()) {
      case CDFDecoder::Status::Idle:
        decode_job->DecodeAsync();
        if(!Timer::IsActive())
          Timer::Schedule(std::chrono::milliseconds(300));
        break;
      case CDFDecoder::Status::Complete:
      case CDFDecoder::Status::Error:
        decode_job->Done();
        decode_queue.pop();
        break;
      case CDFDecoder::Status::Busy:
        break;
    }
  }


  if(request_queue.empty() && decode_queue.empty()) {
    Timer::Cancel();
  }
  is_busy = false;
 
}

void SkysightAPIQueue::Clear(const tstring &&msg) {
  Timer::Cancel();
  for(u_int i = 0; i < request_queue.size(); i++){
    std::unique_ptr<SkysightAsyncRequest> job = std::move(request_queue.front());
    if(job->GetStatus() != SkysightRequest::Status::Busy) {
      job->Done();
      job->TriggerNullCallback(msg.c_str());
      request_queue.pop();
    }
  }
}

void SkysightAPIQueue::SetCredentials(const tstring &&_email, const tstring &&_pass) {
  password = _pass;
  email = _email;
}

void SkysightAPIQueue::SetKey(const tstring &&_key, const uint64_t _key_expiry_time) {
  key = _key;
  key_expiry_time = _key_expiry_time;
}

bool SkysightAPIQueue::IsLoggedIn() {
  
  uint64_t now = (uint64_t)BrokenDateTime::NowUTC().ToUnixTimeUTC();

  //Add a 2-minute padding so that token doesn't expire mid-way thru a request
  return ( ((int64_t)(key_expiry_time - now)) > (60*2) );
  
}



void SkysightAPIQueue::OnTimer() {
  Process();
}
