import QtQuick 2.6
import QtQuick.Window 2.2
import Kingdom 1.0

Window {
    title: "Kingdom"
    visible: true
    visibility: Window.FullScreen
    width: 720
    height: 1280
    color: "#17212b"
    contentOrientation: Qt.LandscapeOrientation

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
        visible: !launcher.running || !gameView.hasFrame
        anchors.centerIn: parent
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: launcher.message || "Загрузка…"
        color: "white"
        font.pixelSize: Math.max(24, Math.min(parent.width, parent.height) / 24)
    }
}
