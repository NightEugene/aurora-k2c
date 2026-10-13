import QtQuick 2.6
import QtQuick.Window 2.2
import Sailfish.Silica 1.0
import Kingdom 1.0
Window {
    title: "Kingdom"
    visible: true; visibility: Window.FullScreen; width: 720; height: 1280; color: "#17212b"
    contentOrientation: launcher.running ? Qt.LandscapeOrientation : Qt.PortraitOrientation
    GameView {
        id: gameView
        objectName: "gameView"
        visible: launcher.running
        anchors.centerIn: parent
        width: Math.max(parent.width, parent.height)
        height: Math.min(parent.width, parent.height)
        rotation: parent.width < parent.height ? 90 : 0
    }
    Text {
        visible: launcher.running && !gameView.hasFrame
        anchors.centerIn: parent
        text: "Загрузка…"
        color: "white"
        font.pixelSize: fs * 1.3
    }
    property int fs: Theme.fontSizeMedium
    property int gap: Theme.paddingMedium
    property int inset: Theme.paddingLarge
    property int touchHeight: Theme.itemSizeMedium
    Item {
        visible: !launcher.running
        anchors.fill: parent; anchors.margins: inset
        Column {
            id: header
            width: parent.width
            spacing: gap
            Text { text: "Kingdom"; color: "white"; font.pixelSize: fs * 1.3 }
            Text { text: launcher.folder; color: "#b8cad8"; font.pixelSize: fs; width: parent.width; wrapMode: Text.WrapAnywhere }
            Rectangle { width: parent.width; height: touchHeight; radius: Theme.paddingSmall; color: upArea.pressed ? "#456478" : "#314556"
                Text { anchors.centerIn: parent; text: "↑ В родительскую папку"; color: "white"; font.pixelSize: fs }
                MouseArea { anchors.fill: parent; id: upArea; onClicked: launcher.up() }
            }
        }
        ListView {
            anchors.top: header.bottom; anchors.topMargin: gap
            anchors.bottom: footer.top; anchors.bottomMargin: gap
            width: parent.width; clip: true; model: launcher.entries
            delegate: Rectangle { width: ListView.view.width; height: touchHeight; color: modelData.path === launcher.selected ? "#365c72" : (index % 2 ? "#223340" : "#1b2b36")
                Text { anchors.fill: parent; anchors.margins: inset; verticalAlignment: Text.AlignVCenter; text: (modelData.directory ? "📁 " : "") + modelData.name; color: "white"; font.pixelSize: fs; elide: Text.ElideMiddle }
                MouseArea { anchors.fill: parent; onClicked: launcher.choose(modelData.path) }
            }
        }
        Column {
            id: footer
            anchors.bottom: parent.bottom
            width: parent.width
            spacing: gap
            Text { text: launcher.selected || "Выберите APK"; width: parent.width; color: "#c5dce9"; font.pixelSize: fs; elide: Text.ElideMiddle }
            Rectangle { width: parent.width; height: touchHeight; radius: Theme.paddingSmall; color: skipArea.pressed ? "#365c72" : "#25394a"
                Row {
                    anchors.centerIn: parent
                    spacing: gap
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: fs; height: fs; radius: Theme.paddingSmall
                        color: launcher.skip ? "#367a58" : "transparent"
                        border.color: "white"; border.width: 2
                        Rectangle { visible: launcher.skip; width: parent.width * 0.25; height: 4; color: "white"; x: parent.width * 0.15; y: parent.height * 0.55; rotation: 45 }
                        Rectangle { visible: launcher.skip; width: parent.width * 0.50; height: 4; color: "white"; x: parent.width * 0.30; y: parent.height * 0.45; rotation: -45 }
                    }
                    Text { text: "Пропускать в следующий раз"; color: "white"; font.pixelSize: fs }
                }
                MouseArea { id: skipArea; anchors.fill: parent; onClicked: launcher.skip = !launcher.skip }
            }
            Rectangle { width: parent.width; height: touchHeight; radius: Theme.paddingSmall; color: launchArea.pressed ? "#4c9b72" : "#367a58"
                Text { anchors.centerIn: parent; text: "Запустить игру"; color: "white"; font.pixelSize: fs }
                MouseArea { anchors.fill: parent; id: launchArea; onClicked: launcher.launch() }
            }
            Text { visible: text.length > 0; text: launcher.message; width: parent.width; wrapMode: Text.Wrap; color: "#ffdf99"; font.pixelSize: fs }
        }
    }
}
