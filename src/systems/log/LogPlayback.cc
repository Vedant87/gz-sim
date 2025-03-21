/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "LogPlayback.hh"

#include <gz/msgs/log_playback_stats.pb.h>

#include <set>
#include <string>
#include <unordered_map>

#include <gz/common/Filesystem.hh>
#include <gz/common/Profiler.hh>
#include <gz/fuel_tools/Zip.hh>
#include <gz/math/Pose3.hh>
#include <gz/msgs/Utility.hh>
#include <gz/plugin/RegisterMore.hh>
#include <gz/transport/log/QueryOptions.hh>
#include <gz/transport/log/Log.hh>
#include <gz/transport/log/Message.hh>

#include <sdf/Geometry.hh>
#include <sdf/Mesh.hh>
#include <sdf/Root.hh>

#include "gz/sim/Conversions.hh"
#include "gz/sim/Events.hh"
#include "gz/sim/SdfEntityCreator.hh"
#include "gz/sim/components/Geometry.hh"
#include "gz/sim/components/LogPlaybackStatistics.hh"
#include "gz/sim/components/Material.hh"
#include "gz/sim/components/ParticleEmitter.hh"
#include "gz/sim/components/Pose.hh"
#include "gz/sim/components/World.hh"

using namespace gz;
using namespace sim;
using namespace systems;

/// \brief Private LogPlayback data class.
class gz::sim::systems::LogPlaybackPrivate
{
  /// \brief Extract model resource files and state file from compression.
  /// \return True if extraction was successful.
  public: bool ExtractStateAndResources();

  /// \brief Start log playback.
  /// \param[in] _logPath Path of recorded state to playback.
  /// \param[in] _ecm The EntityComponentManager of the given simulation
  /// instance.
  /// \return True if any playback has been started successfully.
  public: bool Start(EntityComponentManager &_ecm);

  /// \brief Replace URIs of resources in components with recorded path.
  public: void ReplaceResourceURIs(EntityComponentManager &_ecm);

  /// \brief Prepend log path to mesh file path in the SDF element.
  /// \param[in] _uri URI of mesh in geometry
  /// \return String of prepended path.
  public: std::string PrependLogPath(const std::string &_uri);

  /// \brief Updates the ECM according to the given message.
  /// \param[in] _ecm Mutable ECM.
  /// \param[in] _msg Message containing state updates.
  public: void Parse(EntityComponentManager &_ecm,
      const msgs::SerializedState &_msg);

  /// \brief Updates the ECM according to the given message.
  /// \param[in] _ecm Mutable ECM.
  /// \param[in] _msg Message containing state updates.
  public: void Parse(EntityComponentManager &_ecm,
      const msgs::SerializedStateMap &_msg);

  /// \brief A batch of data from log file, of all pose messages
  public: transport::log::Batch batch;

  /// \brief Pointer to gz-transport Log
  public: std::unique_ptr<transport::log::Log> log;

  /// \brief Indicator of whether any playback instance has ever been started
  public: static bool started;

  /// \brief Directory in which to place log file
  public: std::string logPath{""};

  /// \brief Directory to which compressed file is extracted to
  public: std::string extDest{""};

  /// \brief Indicator of whether this instance has been started
  public: bool instStarted{false};

  /// \brief Flag to print finish message once
  public: bool printedEnd{false};

  /// \brief Pointer to the event manager
  public: EventManager *eventManager{nullptr};

  /// \brief Flag for backward compatibility with log files recorded in older
  /// plugin versions that did not record resources. False for older log files.
  public: bool doReplaceResourceURIs{true};

  // \brief Saves which particle emitter emitting components have changed
  public: std::unordered_map<Entity, bool> prevParticleEmitterCmds;
};

bool LogPlaybackPrivate::started{false};

//////////////////////////////////////////////////
LogPlayback::LogPlayback()
  : System(), dataPtr(std::make_unique<LogPlaybackPrivate>())
{
}

//////////////////////////////////////////////////
LogPlayback::~LogPlayback()
{
  if (!this->dataPtr->extDest.empty())
  {
    common::removeAll(this->dataPtr->extDest);
  }
  if (this->dataPtr->instStarted)
    LogPlaybackPrivate::started = false;
}

//////////////////////////////////////////////////
void LogPlaybackPrivate::Parse(EntityComponentManager &_ecm,
    const msgs::SerializedStateMap &_msg)
{
  _ecm.SetState(_msg);
}

//////////////////////////////////////////////////
void LogPlaybackPrivate::Parse(EntityComponentManager &_ecm,
    const msgs::SerializedState &_msg)
{
  _ecm.SetState(_msg);
}

//////////////////////////////////////////////////
void LogPlayback::Configure(const Entity &,
    const std::shared_ptr<const sdf::Element> &_sdf,
    EntityComponentManager &_ecm, EventManager &_eventMgr)
{
  // Get directory paths from SDF
  this->dataPtr->logPath = _sdf->Get<std::string>("playback_path");

  this->dataPtr->eventManager = &_eventMgr;

  // Prepend working directory if path is relative
  this->dataPtr->logPath = common::absPath(this->dataPtr->logPath);

  // Set the entity offset.
  // \todo This number should be included in the log file.
  _ecm.SetEntityCreateOffset(math::MAX_I64 / 2);

  // If path is a file, assume it is a compressed file
  // (Otherwise assume it is a directory containing recorded files.)
  if (common::isFile(this->dataPtr->logPath))
  {
    std::string extension = common::lowercase(this->dataPtr->logPath.substr(
        this->dataPtr->logPath.find_last_of(".") + 1));
    if (extension != "zip")
    {
      gzerr << "Please specify a zip file.\n";
      return;
    }
    if (!this->dataPtr->ExtractStateAndResources())
    {
      gzerr << "Cannot play back files.\n";
      return;
    }
  }

  // Enforce only one playback instance
  if (!LogPlaybackPrivate::started)
  {
    this->dataPtr->Start(_ecm);
  }
  else
  {
    gzwarn << "A LogPlayback instance has already been started. "
      << "Will not start another.\n";
  }
}

//////////////////////////////////////////////////
bool LogPlaybackPrivate::Start(EntityComponentManager &_ecm)
{
  if (LogPlaybackPrivate::started)
  {
    gzwarn << "A LogPlayback instance has already been started. "
      << "Will not start another.\n";
    return true;
  }

  if (this->logPath.empty())
  {
    gzerr << "Unspecified log path to playback. Nothing to play.\n";
    return false;
  }

  // Append file name
  std::string dbPath = common::joinPaths(this->logPath, "state.tlog");
  gzmsg << "Loading log file [" + dbPath + "]\n";
  if (!common::exists(dbPath))
  {
    gzerr << "Log path invalid. File [" << dbPath << "] "
      << "does not exist. Nothing to play.\n";
    return false;
  }

  // Call Log.hh directly to load a .tlog file
  this->log = std::make_unique<transport::log::Log>();
  if (!this->log->Open(dbPath))
  {
    gzerr << "Failed to open log file [" << dbPath << "]" << std::endl;
  }

  // Access all messages in .tlog file
  this->batch = this->log->QueryMessages();
  auto iter = this->batch.begin();

  if (iter == this->batch.end())
  {
    gzerr << "No messages found in log file [" << dbPath << "]" << std::endl;
  }

  // Look for the first SerializedState message and use it to set the initial
  // state of the world. Messages received before this are ignored.
  for (; iter != this->batch.end(); ++iter)
  {
    auto msgType = iter->Type();
    if (msgType == "gz.msgs.SerializedState")
    {
      msgs::SerializedState msg;
      msg.ParseFromString(iter->Data());
      this->Parse(_ecm, msg);
      break;
    }
    else if (msgType == "gz.msgs.SerializedStateMap")
    {
      msgs::SerializedStateMap msg;
      msg.ParseFromString(iter->Data());
      this->Parse(_ecm, msg);
      break;
    }
  }

  msgs::LogPlaybackStatistics logStats;
  auto startTime = convert<msgs::Time>(this->log->StartTime());
  auto endTime = convert<msgs::Time>(this->log->EndTime());
  logStats.mutable_start_time()->set_sec(startTime.sec());
  logStats.mutable_start_time()->set_nsec(startTime.nsec());
  logStats.mutable_end_time()->set_sec(endTime.sec());
  logStats.mutable_end_time()->set_nsec(endTime.nsec());
  components::LogPlaybackStatistics newLogStatComp(logStats);

  auto worldEntity = _ecm.EntityByComponents(components::World());
  if (kNullEntity == worldEntity)
  {
    gzerr << "Missing world entity." << std::endl;
    return false;
  }

  auto currLogStatComp =
    _ecm.Component<components::LogPlaybackStatistics>(worldEntity);

  if (currLogStatComp)
  {
    *currLogStatComp = newLogStatComp;
  }
  else
  {
    _ecm.CreateComponent(worldEntity, newLogStatComp);
  }

  this->ReplaceResourceURIs(_ecm);

  this->instStarted = true;
  LogPlaybackPrivate::started = true;
  return true;
}

//////////////////////////////////////////////////
void LogPlaybackPrivate::ReplaceResourceURIs(EntityComponentManager &_ecm)
{
  // For backward compatibility with log files recorded in older versions of
  //   plugin, do not prepend resource paths with logPath.
  if (!this->doReplaceResourceURIs)
  {
    return;
  }

  // Define equality functions for replacing component uri
  auto uriEqual = [&](const std::string &_s1, const std::string &_s2) -> bool
  {
    return _s1 == _s2;
  };

  auto geoUriEqual = [&](const sdf::Geometry &_g1,
    const sdf::Geometry &_g2) -> bool
  {
    if (_g1.Type() == sdf::GeometryType::MESH &&
      _g2.Type() == sdf::GeometryType::MESH)
    {
      return uriEqual(_g1.MeshShape()->Uri(), _g2.MeshShape()->Uri());
    }
    else
      return false;
  };

  auto matUriEqual = [&](const sdf::Material &_m1,
    const sdf::Material &_m2) -> bool
  {
    return uriEqual(_m1.ScriptUri(), _m2.ScriptUri());
  };

  // Loop through geometries in world. Prepend log path to URI
  // TODO(anyone): When merge forward to Citadel, handle actor skin and
  // animation files
  _ecm.Each<components::Geometry>(
      [&](const Entity &/*_entity*/, components::Geometry *_geoComp) -> bool
  {
    sdf::Geometry geoSdf = _geoComp->Data();
    if (geoSdf.Type() == sdf::GeometryType::MESH)
    {
      std::string meshUri = geoSdf.MeshShape()->Uri();
      std::string newMeshUri;
      if (!meshUri.empty())
      {
        // Make a copy of mesh shape, and change the uri in the new copy
        sdf::Mesh meshShape = sdf::Mesh(*(geoSdf.MeshShape()));
        newMeshUri = this->PrependLogPath(meshUri);
        meshShape.SetUri(newMeshUri);
        geoSdf.SetMeshShape(meshShape);
        _geoComp->SetData(geoSdf, geoUriEqual);
      }
    }

    return true;
  });

  // Loop through materials in world. Prepend log path to URI
  _ecm.Each<components::Material>(
      [&](const Entity &/*_entity*/, components::Material *_matComp) -> bool
  {
    sdf::Material matSdf = _matComp->Data();
    std::string matUri = matSdf.ScriptUri();
    std::string newMatUri;
    if (!matUri.empty())
    {
      newMatUri = this->PrependLogPath(matUri);
      matSdf.SetScriptUri(newMatUri);
      _matComp->SetData(matSdf, matUriEqual);
    }

    return true;
  });
}

//////////////////////////////////////////////////
std::string LogPlaybackPrivate::PrependLogPath(const std::string &_uri)
{
  // For backward compatibility with log files recorded in older versions of
  // plugin, do not prepend resource paths with logPath.
  if (!this->doReplaceResourceURIs)
  {
    return std::string(_uri);
  }

  const std::string filePrefix = "file://";

  // Prepend if path starts with file:// or /, but recorded path has not
  // already been prepended.
  if (((_uri.compare(0, filePrefix.length(), filePrefix) == 0) &&
      (_uri.substr(filePrefix.length()).compare(
        0, this->logPath.length(), this->logPath) != 0))
      || _uri[0] == '/')
  {
    std::string pathNoPrefix;
    if (_uri[0] == '/')
    {
      pathNoPrefix = std::string(_uri);
    }
    else
    {
      pathNoPrefix = _uri.substr(filePrefix.length());
    }

    // Prepend log path to file path
    std::string pathPrepended = common::joinPaths(this->logPath,
      pathNoPrefix);

    // For backward compatibility. If prepended record path does not exist,
    // then do not prepend logPath. Assume recording is from an older version.
    if (!common::exists(pathPrepended))
    {
      this->doReplaceResourceURIs = false;
      return std::string(_uri);
    }
    else
    {
      return filePrefix + pathPrepended;
    }
  }
  else
  {
    return std::string(_uri);
  }
}

//////////////////////////////////////////////////
bool LogPlaybackPrivate::ExtractStateAndResources()
{
  // Create a temporary directory to extract compressed file content into
  this->extDest = std::string(this->logPath);
  size_t extIdx = this->logPath.find_last_of('.');
  if (extIdx != std::string::npos)
    this->extDest = this->logPath.substr(0, extIdx);
  this->extDest += "_extracted";
  this->extDest = common::uniqueDirectoryPath(this->extDest);

  if (fuel_tools::Zip::Extract(this->logPath, this->extDest))
  {
    gzmsg << "Extracted recording to [" << this->extDest << "]" << std::endl;

    // Replace value in variable with the directory of extracted files
    // Assume directory has same name as compressed file, without extension
    // Remove extension
    this->logPath = common::joinPaths(this->extDest,
      common::basename(this->logPath.substr(0, extIdx)));
    return true;
  }
  else
  {
    gzerr << "Failed to extract recording to [" << this->extDest << "]"
      << std::endl;
    return false;
  }
}

//////////////////////////////////////////////////
void LogPlayback::Reset(const UpdateInfo &, EntityComponentManager &)
{
  // In this case, Reset is a noop
  // LogPlayback already has handling for jumps in time as part of the
  // Update method.
  // Leaving this function implemented but empty prevents the SystemManager
  // from trying to destroy and recreate the plugin.
}

//////////////////////////////////////////////////
void LogPlayback::Update(const UpdateInfo &_info, EntityComponentManager &_ecm)
{
  GZ_PROFILE("LogPlayback::Update");
  if (_info.dt == std::chrono::steady_clock::duration::zero())
    return;

  if (!this->dataPtr->instStarted)
    return;

  // Get all messages from this timestep
  // TODO(anyone) Jumping forward can be expensive for long jumps. For now,
  // just playing every single step so we don't miss insertions and deletions.
  auto startTime = _info.simTime - _info.dt;
  auto endTime = _info.simTime;

  bool seekRewind = false;
  std::set<Entity> entitiesToRemove;
  if (_info.dt < std::chrono::steady_clock::duration::zero())
  {
    // Detected jumping back in time. This can be expensive.
    // To rewind / seek backward in time, we also need to play every single
    // step from the beginning so we don't miss insertions and deletions
    // This is because each serialized state is a changed state and not an
    // absolute state.
    // todo(anyone) Record absolute states during recording, i.e. key frames,
    // so that playback can jump to these states without the need to
    // incrementally build the states from the beginning

    // Create a list of entities to be removed. The list will be updated later
    // as the log steps forward below
    seekRewind = true;
    const auto &entities = _ecm.Entities().Vertices();
    for (const auto &entity : entities)
      entitiesToRemove.insert(Entity(entity.first));

    startTime = std::chrono::steady_clock::duration::zero();
  }

  this->dataPtr->batch = this->dataPtr->log->QueryMessages(
      transport::log::AllTopics({startTime, endTime}));

  auto iter = this->dataPtr->batch.begin();
  while (iter != this->dataPtr->batch.end())
  {
    auto msgType = iter->Type();

    if (msgType == "gz.msgs.SerializedState")
    {
      msgs::SerializedState msg;
      msg.ParseFromString(iter->Data());

      // For seeking back in time only:
      // While stepping, update the list of entities to be removed
      // so we do not remove any entities that are to be created
      if (seekRewind)
      {
        for (const auto &entIt : msg.entities())
        {
          Entity entity{entIt.id()};
          if (entIt.remove())
          {
            entitiesToRemove.insert(entity);
          }
          else
          {
            entitiesToRemove.erase(entity);
          }
        }
      }

      this->dataPtr->Parse(_ecm, msg);
    }
    else if (msgType == "gz.msgs.SerializedStateMap")
    {
      msgs::SerializedStateMap msg;
      msg.ParseFromString(iter->Data());

      // For seeking back in time only:
      // While stepping, update the list of entities to be removed
      // so we do not remove any entities that are to be created
      if (seekRewind)
      {
        for (const auto &entIt : msg.entities())
        {
          const auto &entityMsg = entIt.second;
          Entity entity{entityMsg.id()};
          if (entityMsg.remove())
          {
            entitiesToRemove.insert(entity);
          }
          else
          {
            entitiesToRemove.erase(entity);
          }
        }
      }

      this->dataPtr->Parse(_ecm, msg);
    }
    else if (msgType == "gz.msgs.StringMsg")
    {
      // Do nothing, we assume this is the SDF string
    }
    else
    {
      gzwarn << "Trying to playback unsupported message type ["
              << msgType << "]" << std::endl;
    }
    this->dataPtr->ReplaceResourceURIs(_ecm);
    ++iter;
  }

    // particle emitters
  _ecm.Each<components::ParticleEmitterCmd>(
      [&](const Entity &_entity,
          const components::ParticleEmitterCmd *_emitter) -> bool
  {
    if (this->dataPtr->prevParticleEmitterCmds.find(_entity) ==
        this->dataPtr->prevParticleEmitterCmds.end())
    {
      this->dataPtr->prevParticleEmitterCmds[_entity]
          = _emitter->Data().emitting().data();
      return true;
    }

    if (this->dataPtr->prevParticleEmitterCmds[_entity] !=
        _emitter->Data().emitting().data())
    {
      this->dataPtr->prevParticleEmitterCmds[_entity]
          = _emitter->Data().emitting().data();
      _ecm.SetChanged(_entity, components::ParticleEmitterCmd::typeId,
          ComponentState::OneTimeChange);
    }

    return true;
  });

  // for seek back in time only
  // remove entities that should not be present in the current time step
  for (auto entity : entitiesToRemove)
  {
    _ecm.RequestRemoveEntity(entity);
  }

  // pause playback if end of log is reached
  if (_info.simTime >= this->dataPtr->log->EndTime())
  {
    gzmsg << "End of log file reached. Time: " <<
      std::chrono::duration_cast<std::chrono::seconds>(
      this->dataPtr->log->EndTime()).count() << " seconds" << std::endl;

    this->dataPtr->eventManager->Emit<events::Pause>(true);
  }
}

GZ_ADD_PLUGIN(LogPlayback,
              System,
              LogPlayback::ISystemConfigure,
              LogPlayback::ISystemReset,
              LogPlayback::ISystemUpdate)

GZ_ADD_PLUGIN_ALIAS(LogPlayback,
                          "gz::sim::systems::LogPlayback")
