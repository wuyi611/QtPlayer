/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https: //www.qt.io/licensing/
**
** This file is part of Qt Quick Studio Components.
**
** $QT_BEGIN_LICENSE: GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https: //www.qt.io/terms-conditions. For further
** information use the contact form at https: //www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https: //www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

// ============================================================================
// 导入模块
// ============================================================================
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt.labs.platform as LabPlatform
import HurricanePlayer              // 音视频播放核心组件
import Thumbnail                     // 缩略图组件
import Controller                    // 媒体库控制器
import "./interfacefunctions.js" as IF  // 界面辅助函数
import ponyplayer.ns 1.0            // PonyPlayer 命名空间（错误码等）

// ============================================================================
// 主窗口 - PonyPlayer 播放器根组件
// ============================================================================
Window {
    // Material 主题设置：跟随系统主题，强调色为灰色
    Material.theme: Material.System
    Material.accent: Material.Grey

    // --------------------------------------------------------------------------
    // 工具函数
    // --------------------------------------------------------------------------

    // 将 RGB 分量值转换为十六进制颜色字符串（如 rgb(255,0,0) → "#FF0000"）
    function rgb(r, g, b)
    {
        var ret = (r << 16 | g << 8 | b)
        return ("#"+ret.toString(16)).toUpperCase();
    }

    id: mainWindow

    // ==========================================================================
    // 播放器状态属性
    // ==========================================================================

    // 是否自动连播
    property bool serialize: true
    // 是否是全屏
    property bool isFullScreen: false
    // 音视频播放列表是否可视
    property bool isVideoListOpen: true
    // 音视频操作栏是否可视
    property bool isFooterVisible: true
    // 标题栏是否可见
    property bool isTopBarVisible: true
    // 音视频是否在播放
    property bool isPlay: false
    // 音视频的当前时间（秒）
    property real currentTime: 0.0
    // 音视频的总时长（秒）
    property real endTime: 0.0
    // 播放倍速
    property real speed: 1.0
    // 播放音量（0.0 ~ 1.0）
    property real volumn: 0.5
    // 静音前的音量备份（用于恢复音量）
    property real beforeMute: 0.25
    // 是否倒放
    property bool isInverted: false
    // 播放器界面的当前宽度
    property int userWidth: 900
    // 播放器界面的当前高度
    property int userHeight: 600
    // 播放模式（ordered: 顺序播放, shuffle: 随机播放 等）
    property string playState: "ordered"
    // 亮度（-1.0 ~ 1.0）
    property real brightness: 0.0
    // 饱和度（0.0 ~ 2.0）
    property real saturation: 1.0
    // 对比度（0.0 ~ 2.0）
    property real contrast: 1.0
    // 音轨列表（中转站，由 C++ 层填充 MenuItem）
    property var trackMenu
    // 当前音轨名称
    property string audioTrack:""
    // 最近打开的文件列表（中转站）
    property var currentFilePathStation
    // 输出设备选择（中转站，由 C++ 层填充）
    property var devicesMenuStation
    // 鼠标移动容错标志：防止全屏下鼠标移动时频繁触发显示/隐藏
    property bool mouseFlag: true
    // ==========================================================================
    // 信号定义 - 主窗口向外部（C++ 层 / 其他组件）发送的事件
    // ==========================================================================

    // 开始播放
    signal start()
    // 暂停播放
    signal stop()
    // 切换到下一个媒体
    signal nextOne()
    // 切换到上一个媒体
    signal lastOne()
    // 停止播放
    signal cease()
    // 切换倒放
    signal inverted()
    // 音量改变
    signal volumnChange(real vol)
    // 播放进度改变
    signal currentTimeChange(real cur)
    // 播放模式改变（ordered / shuffle 等）
    signal playModeChange(string state)
    // 播放倍速改变
    signal setSpeed(real speed)
    // 打开指定路径的文件
    signal openFile(string path)
    // 唤醒滑动条更新
    signal wakeSlide()
    // 窗口失去焦点
    signal mainWindowLostFocus()

    // ==========================================================================
    // 子窗口 & 辅助组件
    // ==========================================================================

    // 媒体信息弹窗 — 显示当前播放文件的详细元数据
    MediaInfo {
        id: mediainfowindow
        Component.onCompleted: {
            console.log("mediainfo complete")
        }
    }

    // 定时检测窗口尺寸变化（每秒更新一次）
    Timer{
        id: detechSize
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            mainWindow.userWidth=mainWindow.width
            mainWindow.userHeight=mainWindow.height
        }
    }

    // 滤镜设置弹窗
    FiltersWindow{
        id: filterswindow
    }

    // 操作失败提示弹窗
    Window{
        id: operationFailedWindow
        title: "操作失败"
        width: 200
        height: 150
        Rectangle{
            anchors.fill: parent
            color: "#666666"
            Text{
                id: operationFailedDialogText
                text: "打开文件失败，请选择正确路径"
                anchors.centerIn: parent
                font.bold: true
                font.italic: true
                color: "white"
            }
        }
    }

    // ==========================================================================
    // 窗口基本属性
    // ==========================================================================
    width: 900
    height: 600
    minimumWidth: 750
    minimumHeight: 500
    visible: true
    title: "PonyPlayer"
    // macOS 使用原生窗口装饰，其他平台使用无边框窗口（自绘标题栏）
    flags: (Qt.platform.os=="osx")? Qt.Window: (Qt.Window | Qt.FramelessWindowHint)

    // ==========================================================================
    // 快捷键
    // ==========================================================================

    // F2: 测试热重载崩溃
    Shortcut {
        sequence: "F2"
        onActivated: {
            console.log("Try crash program.")
            hotLoader.crash();
        }
    }
    // F5: 热重载
    Shortcut {
        sequence: "F5"
        onActivated: {
            hotLoader.reload();
        }
    }
    // F1: 测试崩溃
    Shortcut {
        sequence: "F1"
        onActivated: {
            hotLoader.crash();
        }
    }


    // ==========================================================================
    // 全屏交互层 — 全屏时检测鼠标移动，自动显示/隐藏 UI 组件
    // ==========================================================================

    // 全局鼠标区域：全屏下鼠标移动时显示所有 UI 组件并重置自动隐藏计时器
    MouseArea{
        anchors.fill: parent
        id: mainScreen
        hoverEnabled: true // 必须开启 hover 才能检测鼠标移动
        onPositionChanged: {
            if(mainWindow.isFullScreen)
            {
                holder.restart()          // 重置 3 秒倒计时
                mainWindow.isVideoListOpen=true
                mainWindow.isFooterVisible=true
                mainWindow.isTopBarVisible=true
            }
        }
    }

    // 全屏 UI 自动隐藏计时器：全屏下 3 秒无鼠标移动即隐藏工具栏/列表/标题栏
    Timer {
        id: holder
        interval: 3000
        repeat: false
        running: mainWindow.isFullScreen
        triggeredOnStart: false
        onTriggered: IF.hideComponents()
    }
    // ==========================================================================
    // 顶部标题栏 — 自绘标题栏（非 macOS 平台），含菜单和窗口控制按钮
    // ==========================================================================
    Rectangle{
        id: topBar
        anchors.left: leftSizeChange.right
        anchors.right: rightSizeChange.left
        anchors.top: topSizeChange.bottom
        height: mainWindow.isTopBarVisible?30: 0
        visible: mainWindow.isTopBarVisible
        color: "#666666"

        // 标题栏拖动区域 — 点击可拖动窗口
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton //只处理鼠标左键
            property point clickPos: "0, 0"
            onPressed: {
                mainWindow.startSystemMove();  // 触发系统级窗口拖动
            }
        }
        Shortcut{
            sequence: "Ctrl+I"
            onActivated: fileDialog.open();
        }
        // 标题栏左侧的 "PonyPlayer" 文字按钮 — 点击弹出主菜单
        Rectangle{
            id: innerBar
            width: 80
            height: 30
            color: "transparent"
            anchors.left: parent.left
            anchors.leftMargin: 4
            Text {
                text: qsTr("PonyPlayer")
                color: "white"
                font.bold: true
                anchors.centerIn: parent
            }
            MouseArea{
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse)=> {
                if (mouse.button == Qt.RightButton)
                {
                    // 右键预留功能
                }
                else{
                    menu.open()    // 左键打开主菜单
                }
            }
            onEntered: {
                innerBar.color="#10FFFFFF"   // 鼠标悬停高亮
            }
            onExited: innerBar.color="transparent"
        }

        // ======================================================================
        // 主菜单系统
        // ======================================================================
        Menu {
            id: menu
            width: 120
            topMargin: parent.height

            // 文件菜单
            Menu{
                id:fileMenu
                title: qsTr("文件")
                Action {
                    text: "打开文件"
                    onTriggered: fileDialog.open()
                }
                Action {
                    text: "打开链接"
                    onTriggered: openLinkDialog.open()
                }
                Menu{
                    id: currentFilePathList
                    title: qsTr("最近打开的文件")
                    Instantiator {
                        id: recentInstantiator_
                        // controller.h定义
                        model: mediaLibController.recentFiles
                        delegate: MenuItem {
                            text: model.modelData[0]
                            checked: false
                            onTriggered: videoArea.openFile(model.modelData[1])
                        }
                        onObjectAdded: currentFilePathList.insertItem(index, object)
                        onObjectRemoved: currentFilePathList.removeItem(object)
                    }
                }
            }
            // 播放菜单
            Menu {
                id: playMenu
                title: qsTr("播放")
                SpeedMenu{}
                MenuItem {
                    text: (mainWindow.isInverted ? '✔' : '    ') + "倒放"
                    // hurricane.hpp定义
                    checked: videoArea.backwardStatus
                    onTriggered: {
                        mainWindow.isInverted = (!mainWindow.isInverted)
                        videoArea.toggleBackward()
                    }
                }
                MenuItem {
                    id: serializeMenu
                    text: (mainWindow.serialize ? '✔' : '    ') + qsTr("自动连播")
                    checked: mainWindow.serialize
                    onTriggered: {
                        mainWindow.serialize = !mainWindow.serialize
                    }
                }
            }
            // 画面菜单
            Menu{
                title: qsTr("画面")
                Action {
                    text: "播放设置"
                    onTriggered: additionalSettings.show()
                }
                Action{
                    text: "滤镜选择"
                    onTriggered: filterswindow.show()
                }
                Menu {
                    title: "画面比例"
                    MenuItem {
                        text: "保持比例 "+(videoArea.keepFrameRate ? '✔' : '    ')
                        // fireworks.hpp定义
                        checked: videoArea.keepFrameRate
                        onTriggered: {
                            videoArea.keepFrameRate = true
                        }
                    }
                    MenuItem {
                        text: "拉伸画面 "+(!videoArea.keepFrameRate ? '✔' : '    ')
                        checked: !videoArea.keepFrameRate
                        onTriggered: {
                            videoArea.keepFrameRate = false
                        }
                    }
                }
            }
            // 音频菜单
            Menu{
                title: qsTr("音频")
                Menu {
                    id: devicesMenu
                    title: qsTr("输出设备")
                    Instantiator {
                        id: audioInstantiator
                        // hurricane.hpp定义
                        model: videoArea.audioDeviceList
                        delegate: MenuItem {
                            text: (model.modelData === videoArea.currentOutputDevice ? "✔": "") + model.modelData
                            onTriggered: videoArea.setCurrentOutputDevice(model.modelData)
                        }
                        onObjectAdded: (index, object)=> {devicesMenu.insertItem(index, object)}
                        onObjectRemoved: devicesMenu.removeItem(object)
                    }
                }
                Menu {
                    id: trackmenu
                    title: "音轨"
                }

            }
            //当menu加载完后，读取json文件内容，动态添加menuItem
            Component.onCompleted: {
                IF.loadingFilters()
                IF.makeFileList()
            }
        }
    }
    // ==========================================================================
    // 窗口控制按钮 — 关闭 / 最大化/还原 / 最小化
    // ==========================================================================

    // 关闭按钮
    AnimatedButton {
        id: mainWindowClose
        width: 40
        height: 30
        color: "transparent"
        normalColor: "transparent"
        hoverColor: "red"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        image_width: 10
        image_height: 10
        imageSource: "interfacepics/mainWindowClose"
        mouseArea.onClicked: mainWindow.close()
    }
    // 最大化/还原按钮
    AnimatedButton{
        id: mainWindowReduction
        width: 40
        height: 30
        color: "transparent"
        normalColor: "transparent"
        hoverColor: "#10FFFFFF"
        anchors.right: mainWindowClose.left
        anchors.verticalCenter: parent.verticalCenter
        image_width: 10
        image_height: 10
        imageSource: "interfacepics/mainWindowMaximize"
        mouseArea.onClicked: {
            mainWindow.isFullScreen=false
            if(mainWindow.visibility==2)
            {
                mainWindow.visibility=4       // 窗口化
                mainWindowReduction.imageSource="interfacepics/mainWindowReduction"
            }
            else{
                mainWindow.visibility=2       // 最大化
                mainWindowReduction.imageSource="interfacepics/mainWindowMaximize"
            }
        }
    }
    // 最小化按钮
    AnimatedButton{
        id: mainWindowMinimize
        width: 40
        height: 30
        color: "transparent"
        normalColor: "transparent"
        hoverColor: "#10FFFFFF"
        anchors.right: mainWindowReduction.left
        anchors.verticalCenter: parent.verticalCenter
        image_width: 10
        image_height: 10
        imageSource: "interfacepics/mainWindowMinimize"
        mouseArea.onClicked: mainWindow.lower()
    }
}

// 播放设置弹窗组件
AdditionalSettings{
    id: additionalSettings
}

// ============================================================================
// 主体区域 — 包含播放列表、视频播放区、音频可视化
// ============================================================================
Rectangle {
    id: body
    anchors.top: topBar.bottom
    anchors.left: leftSizeChange.right
    anchors.right: rightSizeChange.left
    anchors.bottom: footer.top

    // ========================================================================
    // 右侧播放列表栏
    // ========================================================================
    Rectangle{
        id: videoList
        width: mainWindow.isVideoListOpen?200: 0
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        color: rgb(45, 48, 50)

        // 媒体库控制器 — 与 C++ 层交互，管理播放列表数据
        Controller {
            id: mediaLibController

            onFinishExtractItems: {
                var items = mediaLibController.getSimpleListItemList()

                for(var i=0;i<items.length;i++) {
                listModel.append({"fileName": items[i].getFileName(),
                "filePath": items[i].getFilePath(),
                "iconPath": items[i].getIconPath()==="" ? "interfacepics/defaultlogo": items[i].getIconPath()})
            }
        }

        onFinishGetInfo: {
            var infoitem = mediaLibController.getListItemInfo()
            mediainfowindow.infomodel.clear()
            for(var infokey in infoitem) {
            mediainfowindow.infomodel.append({"infokey": infokey+": ", "infocontent": infoitem[infokey]})
        }
    }
}

        // ====================================================================
        // 播放列表操作栏 — 包含关闭列表按钮
        // ====================================================================
        Item{
            id: videoListOperator
            height: 20
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            // 收起/关闭播放列表栏按钮
            Image {
                id: minimize
                source: "interfacepics/Minimize"
                width: 20
                height: 20
                anchors.top: parent.top
                anchors.topMargin: 5
                anchors.right: parent.right
                anchors.rightMargin: 5
                MouseArea{
                    anchors.fill: parent
                    cursorShape: "PointingHandCursor"
                    onClicked: {
                        mainWindow.isVideoListOpen=false
                    }
                }
            }
        }

        // ====================================================================
        // 播放列表项代理 — 每条媒体项的显示模板
        // ====================================================================
        Component {
            id: listDelegate
            Item {
                id: listitem

                height: listview.height / 10    // 每页显示 10 个
                width: listview.width

                // 单行布局：缩略图 + 文件名 + 删除按钮
                Item {
                    id: rowlayout
                    anchors.top: parent.top
                    anchors.right: deleteitem.left
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom

                    // 缩略图
                    Image {
                        id: preview
                        source: iconPath
                        anchors.left: parent.left
                        anchors.leftMargin: parent.height*0.1
                        height: parent.height*0.8
                        width: height
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // 文件名（过长时中间省略）
                    Text {
                        id: filaname
                        text: fileName
                        elide: Text.ElideMiddle
                        font.bold: true
                        anchors.left: preview.right
                        width: parent.width - preview.width
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        color: rgb(173, 173, 173)
                    }
                    // 隐藏的文件路径（用于数据传递）
                    Text {
                        id: fpath
                        text: filePath
                        visible: false
                        width: 0
                    }

                    // 点击播放 / 双击查看媒体信息
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: "PointingHandCursor"

                        onClicked: {
                            listview.currentIndex = index;
                            console.log("[P]selected file: "+listModel.get(index).filePath)
                            mainWindow.openFile(listModel.get(index).filePath);
                            wave.waveArea.tryLoadLyrics(listModel.get(index).filePath);
                        }

                        onDoubleClicked: {
                            mediaLibController.sendGetInfoRequirement(listModel.get(index).filePath)
                            mediainfowindow.show()
                        }
                    }
                }

                // 从列表中移除该项的删除按钮
                Image {
                    id: deleteitem
                    height: preview.height*0.5
                    width: preview.width*0.5
                    source: "interfacepics/FileCloser"
                    anchors.right: parent.right
                    anchors.rightMargin: parent.height*0.1
                    anchors.verticalCenter: parent.verticalCenter

                    MouseArea{
                        anchors.fill: parent
                        cursorShape: "PointingHandCursor"

                        onClicked: {
                            console.log("Image")
                            mediaLibController.sendRemoveRequirement(listModel.get(index).filePath, listModel.get(index).iconPath)
                            listModel.remove(index, 1)
                        }
                    }
                }
            }
        }

        // 可滚动的播放列表视图
        ScrollView{
            id: videoScroll
            anchors.top: videoListOperator.bottom
            anchors.topMargin: 10
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.left: parent.left
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ListView {
                anchors.right: parent.right
                anchors.left: parent.left
                id: listview
                focus: true
                model: ListModel{
                    id: listModel
                }
                highlight: Rectangle {
                    color: "#10FFFFFF"
                }
                delegate: listDelegate
            }
        }
        // 播放列表加载完成后，从 C++ 层提取媒体库数据
        Component.onCompleted: {
            mediaLibController.sendExtractRequirement()
        }
}

    // ========================================================================
    // 中央媒体播放区域 — 使用 SwipeView 切换视频/初始画面/音频可视化
    // ========================================================================
    SwipeView{
        id: mainArea
        orientation: Qt.Horizontal
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: videoList.left
        interactive: false    // 禁止手动滑动切换
        currentIndex: 1       // 默认显示初始画面（index 1）
        clip: true

        // 页面 0: 视频播放画面
        Rectangle{
            color: "black"
            HurricanePlayer{
                id: videoArea
            clip: true
            anchors.fill: parent

            // 视频区域鼠标检测 — 全屏下移动鼠标显示 UI 组件
            MouseArea{
                anchors.fill: parent
                hoverEnabled: true
                propagateComposedEvents: true
                onPositionChanged: {
                    if(mainWindow.isFullScreen)
                    {
                        if(mainWindow.mouseFlag)
                        {
                            mainWindow.mouseFlag=false   // 首次移动忽略（防抖）
                        }
                        else{
                            IF.showComponents()           // 显示所有 UI 组件
                        }
                    }
                }
            }

            // 当前媒体播放完毕 → 自动连播下一个
            onResourcesEnd: {
                if(mainWindow.serialize) {
                    IF.nextOnClicked();
                }
            }
            // 播放状态变化时同步 UI
            onStateChanged: IF.solveStateChanged()
            // 播放器初始化
            Component.onCompleted: IF.mainAreaInit()

            // 处理打开文件的结果回调
            onOpenFileResult: (result)=> {
            if(result == PonyPlayerNS.FAILED)
            {
                // 打开失败：显示错误弹窗
                operationFailedDialogText.text="文件不存在或文件格式不支持"
                operationFailedWindow.show()
                mainWindow.endTime=0
            }
            else if(result == PonyPlayerNS.VIDEO)
            {
                // 视频文件：切换到视频播放页面
                mainArea.currentIndex = 0;
                IF.toVideoBegining()
                mainWindow.endTime=Math.floor(videoArea.getAudioDuration())
                IF.makeTrackMenu()
                if (mainWindow.isInverted)
                {
                    mainWindow.isInverted = false
                }
                if(mainWindow.speed=8.0)
                {
                    mainWindow.speed=1.0
                    videoArea.setSpeed(mainWindow.speed)
                }
                IF.judgeSerialize()
            }
            else if(result == PonyPlayerNS.AUDIO){
            // 音频文件：切换到音频可视化页面
            mainArea.currentIndex = 2;
            IF.toVideoBegining()
            mainWindow.endTime=Math.floor(videoArea.getAudioDuration());
            IF.makeTrackMenu()
            if (mainWindow.isInverted)
            {
                mainWindow.isInverted = false
                videoArea.forward();
            }
            IF.judgeSerialize()
        }
    }

}
}
// 页面 1: 初始画面 — 默认显示的欢迎页面
Rectangle{
    id: initScreen
    color: "#4e4e4e"
    Image{
        id: initImage
        width: 200
        height: 200
        source: "interfacepics/ponyback"
        anchors.centerIn: parent
    }
    // 全屏鼠标检测（同视频页面逻辑）
    MouseArea{
        anchors.fill: parent
        hoverEnabled: true //默认是false
        propagateComposedEvents: true
        onPositionChanged: {
            if(mainWindow.isFullScreen)
            {
                if(mainWindow.mouseFlag)
                {
                    mainWindow.mouseFlag=false
                }
                else{
                    IF.showComponents()
                }
            }
        }
    }
    Button {
        anchors.top: initImage.bottom
        anchors.horizontalCenter: initImage.horizontalCenter
        text: "打开文件"
        width: 120
        height: 30
        onClicked: fileDialog.open()

    }
}
// 页面 2: 音频可视化 — 播放纯音频时显示波形图
Wave{
    id: wave
    // 全屏鼠标检测（同视频页面逻辑）
    MouseArea{
        anchors.fill: parent
        hoverEnabled: true //默认是false
        propagateComposedEvents: true
        onPositionChanged: {
            if(mainWindow.isFullScreen)
            {
                if(mainWindow.mouseFlag)
                {
                    mainWindow.mouseFlag=false
                }
                else{
                    IF.showComponents()
                }
            }
        }
    }
}
}
}

// ============================================================================
// 底部控制栏 — 播放/暂停、进度条、音量等控制组件
// ============================================================================
PonyFooter{
    id: footer
    height: mainWindow.isFooterVisible?80: 0
    visible: mainWindow.isFooterVisible
    anchors.left: leftSizeChange.right
    anchors.right: rightSizeChange.left
    anchors.bottom: downSizeChange.top
}

// ============================================================================
// 窗口尺寸调整手柄 — 8 个方向的拖拽调整区域（用于无边框窗口的自定义 resize）
// 命名规则：{方位}SizeChange → 对应上/下/左/右/四角的拖拽手柄
// ============================================================================

// 顶部中央
TopSizeChange{
    id: topSizeChange
    anchors.left: leftTopSizeChange.right
    anchors.right: rightTopSizeChange.left
    anchors.top: parent.top
}
// 左侧中央
LeftSizeChange{
    id: leftSizeChange
    anchors.top: leftTopSizeChange.bottom
    anchors.left: parent.left
    anchors.bottom: downSizeChange.top
}
// 左下角
LeftDownSizeChange{
    id: leftDownSizeChange
    anchors.bottom: parent.bottom
    anchors.left: parent.left
}
// 底部中央
DownSizeChange{
    id: downSizeChange
    anchors.bottom: parent.bottom
    anchors.left: leftSizeChange.right
    anchors.right: rightSizeChange.left
}
// 右下角
RightDownSizeChange{
    id: rightDownSizeChange
    anchors.bottom: parent.bottom
    anchors.right: parent.right
}
// 左上角
LeftTopSizeChange{
    id: leftTopSizeChange
    anchors.top: parent.top
    anchors.left: parent.left
}
// 右侧中央
RightSizeChange{
    id: rightSizeChange
    anchors.right: parent.right
    anchors.top: rightTopSizeChange.bottom
    anchors.bottom: downSizeChange.top
}
// 右上角
RightTopSizeChange{
    id: rightTopSizeChange
    anchors.top: parent.top
    anchors.right: parent.right
}

// ============================================================================
// 窗口级别事件处理
// ============================================================================

// 焦点变化时通知外部（用于暂停播放等场景）
onActiveFocusItemChanged: {
    mainWindow.mainWindowLostFocus()
}

// 设置 LUT 滤镜（由外部调用）
function setFilter(lut)
{
    videoArea.setLUTFilter(lut)
}

// 窗口加载完成：macOS 平台特殊处理 — 使用原生标题栏，初始化 DBus
Component.onCompleted: {
    if (Qt.platform.os=="osx")
    {
        let dbusComponent = Qt.createComponent("DBus.qml");
        let dbusItem = dbusComponent.createObject(mainWindow, { id: "dbus" });
        topBar.height = 0;
        topBar.visible = false;
    }
}

// ============================================================================
// 对话框
// ============================================================================

// 文件打开对话框
FileDialog{
    id: fileDialog
    title: "打开文件"
    nameFilters: [ "Media files (*.mp4 *avi *.ts *.rmvb *.mp3 *.wav)", "Video files (*.mp4 *avi *.ts *.rmvb)", "Audio files (*.mp3 *wav)", "All files (*)" ]
    onAccepted: IF.videoListOperatorOnAccepted()
    onRejected: {
        console.log("reject")
    }
}

// 打开链接对话框
Dialog {
    id: openLinkDialog
    title: "打开链接"
    height: 200
    width: 400
    standardButtons: Dialog.Ok | Dialog.Cancel
    focus: true
    modal: false
    anchors.centerIn: parent

    Column {
        anchors.fill: parent

        TextField {
            id: urlInput
            width: parent.width * 0.9
            focus: true
        }
    }
    onAccepted: mainWindow.openFile(urlInput.text)
}
}


