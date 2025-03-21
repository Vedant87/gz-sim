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

import QtQuick 2.9
import QtQuick.Controls 2.1
import QtQuick.Layouts 1.3
import RenderWindowOverlay 1.0
import GzSim 1.0 as GzSim

ColumnLayout {
  Layout.minimumWidth: 350
  Layout.minimumHeight: 370
  anchors.fill: parent
  anchors.margins: 10

  property string message: 'Adding a right-click context menu to the 3D scene.<br><br>' +
      'For proper positioning:<br><ol>' +
      '<li>Undock the menu</li>' +
      '<li>Right-click this text, and choose Settings</li>' +
      '<li>Hide the title bar and set height to zero</li></ol><br><br>' +
      'These other plugins must also be loaded to enable all functionality:<br><ul>' +
      '<li>Move To / Follow: Camera tracking</li>' +
      '<li>Copy / Paste: Copy Paste</li>' +
      '<li>View: Visualization Capabilities</li>' +
      '<li>Remove: Gz Scene Manager</li></ul>'

  Label {
    Layout.fillWidth: true
    wrapMode: Text.WordWrap
    text: message
  }

  Item {
    width: 10
    Layout.fillHeight: true
  }

  RenderWindowOverlay {
    id: renderWindowOverlay
    objectName: "renderWindowOverlay"

    Connections {
      target: renderWindowOverlay
      onOpenContextMenu:
      {
        entityContextMenu.open(_entity, "model",
          _mouseX, _mouseY);
      }
    }
  }

  GzSim.EntityContextMenu {
    id: entityContextMenu
  }
}
