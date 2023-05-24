/*
 * Copyright (C) 2021 Open Source Robotics Foundation
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
#ifndef IGNITION_GAZEBO_COMPONENTS_LABEL_HH_
#define IGNITION_GAZEBO_COMPONENTS_LABEL_HH_

#include "gz/sim/Export.hh"
#include "gz/sim/components/Component.hh"
#include "gz/sim/components/Factory.hh"
#include "gz/sim/config.hh"

namespace ignition
{
namespace gazebo
{
// Inline bracket to help doxygen filtering.
inline namespace IGNITION_GAZEBO_VERSION_NAMESPACE {
namespace components
{
  /// \brief A component that holds the label of an entity. One example use
  /// case of the Label component is with Segmentation & Bounding box
  /// sensors to generate dataset annotations.
  using SemanticLabel = Component<uint32_t, class SemanticLabelTag>;
  IGN_GAZEBO_REGISTER_COMPONENT("ign_gazebo_components.SemanticLabel",
      SemanticLabel)
}
}
}
}
#endif
