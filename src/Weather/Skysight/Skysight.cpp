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

#include "Skysight.hpp"

#include "OS/ConvertPathName.hpp"
#include "OS/Path.hpp"
#include "LocalPath.hpp"
#include "OS/FileUtil.hpp"

#include "Util/StringCompare.hxx"
#include "Util/Macros.hpp"
#include "Util/tstring.hpp"


#include <tchar.h>

#include <string>

#include <vector>

#include "Util/StaticString.hxx"

#include "Profile/Profile.hpp"
#include "ActionInterface.hpp"
  
#include "OS/FileUtil.hpp"
#include "Interface.hpp"
#include "UIGlobals.hpp"

#include "Language/Language.hpp"

#include "LogFile.hpp"

#include "Time/BrokenDateTime.hpp"
//#include "Event/Timer.hpp"

#include <memory>
#include "MapWindow/OverlayBitmap.hpp"
#include "MapWindow/GlueMapWindow.hpp"

#include "Thread/Debug.hpp"


/**
 * TODO:
 * -- overlay only shows following render -- no way to trigger from child thread
 * -- no transparent bg on overlay on android
 * 
 * --- for release ----
 * - Use SkysightImageFile elsewhere instead of recalculating forecast time, move to separate imp file
 * - clean up libs
 * - rebase on latest master, clean up
 * - move cache trimming to API?
 * - clean up metrics/activemetrics/displayed_layer -- inheritance rather than pointer
 * - Add documentation
 * - Test cubie compile / libs
 
 --- style ----
 * fix variable style/case, 
 * reduce use of STL strings
 * Can use AtScopeExit for object cleanup in tiff generation
 * Use consistent string conventions ( _()?,  _T()?s )
 * replace #defines in skysight.hpp with better c++ idioms
* Use static_cast<> instead of c casts
 */

Skysight *Skysight::self;

/*
 * 
 * Img File
 * 
 */


SkysightImageFile::SkysightImageFile(Path _filename) {
  filename = _filename;
  region = tstring(_("INVALID"));
  layer = tstring(_("INVALID"));
  datetime = 0;
  is_valid = false;
  mtime = 0;
  

  //images are in format region-metric-datetime.tif
  
  if(!filename.MatchesExtension(".tif"))
    return;
    
  tstring file_base = filename.GetBase().c_str();
  
  std::size_t p = file_base.find(_("-"));
  if(p == tstring::npos)
    return;
    
  tstring reg = file_base.substr(0, p);  
  tstring rem = file_base.substr(p+1);  
  
  p = rem.find(_("-"));
  if(p == tstring::npos)
    return;    
  tstring layer_tmp = rem.substr(0, p);

  tstring dt = rem.substr(p+1);
  unsigned yy = stoi(dt.substr(0, 4));
  unsigned mm = stoi(dt.substr(4, 2));
  unsigned dd = stoi(dt.substr(6, 2));
  unsigned hh = stoi(dt.substr(8, 2));
  unsigned ii = stoi(dt.substr(10, 2));

  BrokenDateTime d = BrokenDateTime(yy, mm, dd, hh, ii);
  if(!d.IsPlausible())
    return;

  datetime = d.ToUnixTimeUTC();

  mtime = File::GetLastModification( AllocatedPath::Build(MakeLocalPath(_T("skysight")), filename) );

  region = reg;
  layer = layer_tmp;
  is_valid = true;
}

bool Skysight::IsStandbyLayer(const TCHAR *const id) {
  for(auto &i : standby_layers)
    if(!i.descriptor->id.compare(id))
      return true;

  return false;
}


bool Skysight::StandbyLayersFull() {
  return (standby_layers.size() >= SKYSIGHT_MAX_STANDBY_LAYERS);
}

int Skysight::AddStandbyLayer(const TCHAR *const id) {
  bool descriptor_exists = false;
  auto i = api.descriptors.begin();
  for(; i < api.descriptors.end(); ++i)
    if(!i->id.compare(id)) {
      descriptor_exists = true;
      break;
    }
  if(!descriptor_exists)
    return -3;
  
 
  if(IsStandbyLayer(id))
    return -1;
    
  if(StandbyLayersFull())
    return -2;  
  
  SkysightStandbyLayer m = SkysightStandbyLayer(&(*i), 0, 0, 0);

  SetupStandbyLayer(id, m);
    
  standby_layers.push_back(m);
  SaveStandbyLayers();
  return standby_layers.size() - 1;
}


void Skysight::RefreshStandbyLayer(const tstring id) {
  SkysightStandbyLayer *layer = GetStandbyLayer(id);
  if(layer != NULL){
      SetupStandbyLayer(id, *layer);
  }
}

SkysightStandbyLayer Skysight::GetStandbyLayer(unsigned index) {
  assert(index < standby_layers.size());
  auto &i = standby_layers.at(index);
  return i;
}

SkysightStandbyLayer *Skysight::GetStandbyLayer(const tstring id) {
  auto i = standby_layers.begin();
  for(; i<standby_layers.end();++i)
    if(!i->descriptor->id.compare(id)) {
      return &(*i);
    }
  return &(*i);
}

void Skysight::SetStandbyLayerUpdateState(const tstring id, bool state) {
  for(auto &i : standby_layers) {
    if(!i.descriptor->id.compare(id)) {
     i.updating = state;
     return;
    }
  }
}

void Skysight::RemoveStandbyLayer(const tstring id) {
  for(auto i = standby_layers.begin(); i<standby_layers.end(); ++i) {
    if(i->descriptor->id == id)
      standby_layers.erase(i);
  }
  SaveStandbyLayers();
}

bool Skysight::StandbyLayersUpdating() {
  for(auto i : standby_layers)
    if(i.updating) return true;

  return false;
}

unsigned Skysight::GetNumStandbyLayers() {
  return standby_layers.size(); 
}

void Skysight::SaveStandbyLayers() {
  
  tstring am_list;
  
  if(GetNumStandbyLayers()) {
    for(auto &i : standby_layers) {
      am_list += i.descriptor->id;
      am_list += ",";
    }
    am_list.pop_back();
  } else {
    am_list = "";
  }
  
  Profile::Set(ProfileKeys::SkysightStandbyLayers, am_list.c_str());
  
}

void Skysight::LoadStandbyLayers() {
  
  standby_layers.clear();
  
  const char *s = Profile::Get(ProfileKeys::SkysightStandbyLayers);
  if(s == NULL)
    return;
  tstring am_list = tstring(s);
  size_t pos;
  while ((pos = am_list.find(",")) != tstring::npos) {
    AddStandbyLayer(am_list.substr(0, pos).c_str());
    am_list.erase(0, pos + 1);
  }
  AddStandbyLayer(am_list.c_str()); // last one
  const TCHAR *const d = Profile::Get(ProfileKeys::SkysightDisplayedLayer);
  if (d == NULL || *d != '\0')
    return;

  const unsigned saved_displayed_layer = (unsigned)std::stoi(d);

  if (saved_displayed_layer >= SKYSIGHT_MAX_STANDBY_LAYERS)
    return;

  SetSkysightDisplayedLayer(saved_displayed_layer);
}


bool Skysight::IsReady(bool force_update) 
{
  if(email.empty() || password.empty() || region.empty())
    return false;

  return (GetNumLayerDescriptors() > 0);
}


Skysight::Skysight() { 
  self = this;
  Init();
}

Skysight::~Skysight() {
}

void Skysight::Init() {
  const auto settings = CommonInterface::GetComputerSettings().weather.skysight;
  region = settings.region.c_str();
  email = settings.email.c_str();
  password = settings.password.c_str();

  api.Init(email,  password,  region,  APIInited);
  CleanupFiles();
}

void Skysight::APIInited(const tstring details,  const bool success,  
            const tstring layer_id,  const uint64_t time_index) {
  
  if(!self)
    return;
  
  if(self->api.descriptors.size()) {
    self->LoadStandbyLayers();
    self->Render(true);
  }
}

/** Sets up the given standbyLayer m and returns true or, if the layer_name is not found, returns false
 * 
 */
bool Skysight::SetupStandbyLayer(tstring layer_name, SkysightStandbyLayer &m) {
  
  tstring search_pattern = region + "-" + layer_name + "*";
  std::vector<SkysightImageFile> img_files = ScanFolder(search_pattern);
  
  if(img_files.size() > 0) {
    uint64_t min_date = (uint64_t)std::numeric_limits<uint64_t>::max;
    uint64_t max_date = 0;
    uint64_t updated = 0;
    
    for(auto &i : img_files) {
      min_date = std::min(min_date, i.datetime);
      max_date = std::max(max_date, i.datetime);
      updated  = std::max(updated, i.mtime);
    }
    if(GetLayerDescriptorExists(layer_name)) {
      m.descriptor =new SkysightLayerDescriptor(*GetLayer(layer_name));
      m.from = min_date;
      m.to = max_date;
      m.mtime = updated;
      
      return true;
    }
  }
  
  return false;
}

std::vector<SkysightImageFile> Skysight::ScanFolder(tstring search_string = "*.tif") {
  
  //start by checking for output files
  std::vector<SkysightImageFile> file_list;
  
  struct SkysightFileVisitor : public File::Visitor {
    std::vector<SkysightImageFile> &file_list;
    explicit SkysightFileVisitor(std::vector<SkysightImageFile> &_file_list) : file_list(_file_list) {}

    void Visit(Path path, Path filename) override {
      //is this a tif filename
      if(filename.MatchesExtension(".tif")) {
        SkysightImageFile img_file = SkysightImageFile(filename);
        if (img_file.is_valid)
          file_list.emplace_back(img_file);
      }
    }
    
  } visitor(file_list);

  Directory::VisitSpecificFiles(GetLocalPath(), _T(search_string.c_str()), visitor);
  return file_list;

}

void Skysight::CleanupFiles() {
  
  struct SkysightFileVisitor : public File::Visitor {
    explicit SkysightFileVisitor(const uint64_t _to) : to(_to) {}
    const uint64_t to;
    void Visit(Path path, Path filename) override {
      if(filename.MatchesExtension(".tif")) {
        SkysightImageFile img_file = SkysightImageFile(filename);
        if( (img_file.mtime <= (to - (60*60*24*5))) || 
                                            (img_file.datetime < (to - (60*60*24))) ) {
          File::Delete(path);
        }
      } 
    }

  } visitor(Skysight::GetNow().ToUnixTimeUTC());

  Directory::VisitSpecificFiles(GetLocalPath(), _T("*.tif"), visitor);

}


BrokenDateTime Skysight::FromUnixTime(uint64_t t) {
  return api.FromUnixTime(t);
}

void Skysight::Render(bool force_update) {
  if (displayed_layer >= SKYSIGHT_MAX_STANDBY_LAYERS)
    return;
  
  //TODO: Why use if(.descriptor?)
  if(GetStandbyLayer(displayed_layer).descriptor) {
    LogFormat("In Render!");
    if(update_flag) {
      DisplayStandbyLayer(displayed_layer);
    }

    //Request next images
    BrokenDateTime now = Skysight::GetNow(force_update);
    if(force_update || (!update_flag && GetStandbyLayer(displayed_layer).to < (unsigned)GetForecastTime(now).ToUnixTimeUTC())) {
      force_update = false;
      api.GetImageAt(GetStandbyLayer(displayed_layer).descriptor->id.c_str(), now, now + (60*60), DownloadComplete);
    }
  }
}


BrokenDateTime Skysight::GetForecastTime(BrokenDateTime curr_time) {
  
  if(!curr_time.IsPlausible())
    curr_time = Skysight::GetNow();
  
  if((curr_time.minute >= 15) && (curr_time.minute < 45)) curr_time.minute = 30;
  else if(curr_time.minute >= 45) { 
    curr_time.minute = 0;
    curr_time = curr_time + (60*60);
  }
  else if(curr_time.minute < 15) 
    curr_time.minute = 0;
  
  curr_time.second = 0;
  return curr_time;
}
  
  
  

bool Skysight::SetSkysightDisplayedLayer(const unsigned index) {
  
  if(index >= SKYSIGHT_MAX_STANDBY_LAYERS)
    return false;

  displayed_layer = index;

  return true;
}


void Skysight::DownloadComplete(const tstring details,  const bool success,
                const tstring layer_id,  const uint64_t time_index) {

  if(!self) return;

  self->SetStandbyLayerUpdateState(layer_id, false);
  self->RefreshStandbyLayer(layer_id);

  if(success && self->displayed_layer < self->GetNumStandbyLayers() && self->GetStandbyLayer(self->displayed_layer).descriptor->id.c_str() == layer_id.c_str())
    self->update_flag = true; 
}


bool Skysight::DownloadStandbyLayer(tstring id = "*") {

  BrokenDateTime now = Skysight::GetNow();
  if(id == "*") {
    for(auto &i : standby_layers) {
      SetStandbyLayerUpdateState(i.descriptor->id, true);
      api.GetImageAt(i.descriptor->id.c_str(),  now, now + 60*60*24,  DownloadComplete);
    }
  } else {
    SetStandbyLayerUpdateState(id, true);
    api.GetImageAt(id.c_str(),  now, now + 60*60*24,  DownloadComplete);
  }

  return true;
}

void Skysight::OnCalculatedUpdate(const MoreData &basic,
                                  const DerivedInfo &calculated) {
  // maintain current time -- for use in replays etc. 
  // Cannot be accessed directly from chid threads
  curr_time = basic.date_time_utc;
}


BrokenDateTime Skysight::GetNow(bool use_system_time) {
  if(use_system_time)
    return BrokenDateTime::NowUTC();
  
  return (curr_time.IsPlausible()) ? curr_time : BrokenDateTime::NowUTC();;
}

bool Skysight::DisplayStandbyLayer(const unsigned index) {

  update_flag = false;
    
  if(index >= SKYSIGHT_MAX_STANDBY_LAYERS) {
    displayed_layer = SKYSIGHT_MAX_STANDBY_LAYERS;
    auto *map = UIGlobals::GetMap();
    if (map == nullptr)
      return false;

    map->SetOverlay(nullptr);
    Profile::Set(ProfileKeys::SkysightDisplayedLayer, "");
    return true;
  }

  Profile::Set(ProfileKeys::SkysightDisplayedLayer, index);

  BrokenDateTime now = GetForecastTime(Skysight::GetNow());
  
  int offset = 0;
  uint64_t n = now.ToUnixTimeUTC();
  
  
  uint64_t test_time;
  bool found = false;
  NarrowString<256> filename;
  BrokenDateTime bdt; 
  int max_offset = (60*60);
  
  //TODO: We're only searching w a max offset of 1 hr, simplify this!
  
  while(!found) {
    //look back for closest forecast first, then look forward
    for(unsigned i = 0; i <= 1; i++) {
      test_time = n + ( offset * ( (2 * i) - 1));
 
      bdt = FromUnixTime(test_time);
      filename.Format("%s-%s-%04u%02u%02u%02u%02u.tif", 
            region.c_str(), GetStandbyLayer(index).descriptor->id.c_str(),
            bdt.year, bdt.month, 
            bdt.day, bdt.hour, bdt.minute);
                
      if(File::Exists(AllocatedPath::Build(GetLocalPath(), filename.c_str()))) {
        found = true;
        break;
      }
      if(offset == 0) break;
    }
    if(!found)
      offset += (60*30);  

    if(offset > max_offset)
      break;

  }

  if(!found) {
    SetSkysightDisplayedLayer(index);
    LogFormat("Not found with %s", GetStandbyLayer(index).descriptor->id.c_str());
    return false;
  }
  
  if(!SetSkysightDisplayedLayer(index)) {
    LogFormat("SetSkysightDispokla false");
    return false;
  }

  auto path = AllocatedPath::Build(Skysight::GetLocalPath(), filename.c_str());
  StaticString<256> desc;
  desc.Format("Skysight: %s (%04u-%02u-%02u %02u:%02u)", GetStandbyLayer(index).descriptor->name.c_str(), bdt.year, bdt.month, 
            bdt.day, bdt.hour, bdt.minute);
  tstring label = desc.c_str();

  auto *map = UIGlobals::GetMap();
  if (map == nullptr)
    return false;

  LogFormat("Setting %s", path.c_str());
  std::unique_ptr<MapOverlayBitmap> bmp;
  try {
    bmp.reset(new MapOverlayBitmap(path));
  } catch (const std::exception &e) {
    return false;
  }

  bmp->SetAlpha(0.6);
  bmp->SetLabel(label);
  map->SetOverlay(std::move(bmp));
  LogFormat("%s", "Success");
  return true;
}

