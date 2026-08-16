/**
 * 测试函数，用于调试时打印路径信息
 * @param {string} path - 要打印的路径
 */
function mytest(path) {
  console.log(path);
}

/**
 * 动态加载视频滤镜列表
 * 从 videoArea 中读取滤镜 JSON 数据，解析后填充到 filtermodel 中，
 * 包括原始画面（origin）以及 Contrast、Flim、Video 三类滤镜
 */
function loadingFilters() {
  let fileNames = ["Contrast", "Flim", "Video"];
  let prefix = videoArea.filterPrefix;
  let beforePrefix = "file://";
  if (prefix[2] == "/") {
    beforePrefix = beforePrefix + "/";
  }
  // 添加原始画面（无滤镜）选项
  filtermodel.append({
    filterNames: "origin",
    images: beforePrefix + prefix + "/origin.jpg",
    luts: "",
  });
  let jsons = videoArea.filterJsons;
  for (let i = 0; i < jsons.length; i++) {
    var json = JSON.parse(jsons[i]);
    for (let j = 0; j < json.length; j++) {
      filtermodel.append({
        filterNames: fileNames[i] + ":  " + j,
        images: beforePrefix + prefix + "/" + json[j].image,
        luts: json[j].lut,
      });
    }
  }
}

/**
 * 快进 1 秒
 * 如果当前时间未到达视频结尾，则前进 1 秒；否则跳转到视频结尾
 */
function forwardOneSecond() {
  if (mainWindow.endTime == 0.0) {
    return;
  }
  if (mainWindow.endTime > mainWindow.currentTime) {
    mainWindow.currentTime = mainWindow.currentTime + 1.0;
  } else {
    mainWindow.currentTime = mainWindow.endTime;
  }
  videoSlide.value = mainWindow.currentTime;
  videoArea.seek(mainWindow.currentTime);
}

/**
 * 快进 5 秒
 * 如果剩余时间超过 5 秒，则前进 5 秒；否则跳转到视频结尾
 */
function forwardFiveSeconds() {
  if (mainWindow.endTime == 0.0) {
    return;
  }
  if (mainWindow.endTime - mainWindow.currentTime > 5.0) {
    mainWindow.currentTime = mainWindow.currentTime + 5.0;
  } else {
    mainWindow.currentTime = mainWindow.endTime;
  }
  videoSlide.value = mainWindow.currentTime;
  videoArea.seek(mainWindow.currentTime);
}

/**
 * 后退 1 秒
 * 如果当前时间超过 1 秒，则后退 1 秒；否则跳转到视频开头
 */
function backOneSecond() {
  if (mainWindow.currentTime == 0.0) {
    return;
  }
  if (mainWindow.currentTime > 1.0) {
    mainWindow.currentTime = mainWindow.currentTime - 1.0;
  } else {
    mainWindow.currentTime = 0.0;
  }
  videoSlide.value = mainWindow.currentTime;
  videoArea.seek(mainWindow.currentTime);
}

/**
 * 后退 5 秒
 * 如果当前时间超过 5 秒，则后退 5 秒；否则跳转到视频开头
 */
function backFiveSeconds() {
  if (mainWindow.currentTime == 0.0) {
    return;
  }
  if (mainWindow.currentTime > 5.0) {
    mainWindow.currentTime = mainWindow.currentTime - 5.0;
  } else {
    mainWindow.currentTime = 0.0;
  }
  videoSlide.value = mainWindow.currentTime;
  videoArea.seek(mainWindow.currentTime);
}

/**
 * 增加音量
 * 每次增加 0.1（即 10%），最大为 1.0（100%）
 * 同时更新静音前的音量记录（beforeMute）
 */
function volumnUp() {
  if (mainWindow.volumn < 0.9) {
    mainWindow.volumn = mainWindow.volumn + 0.1;
    mainWindow.beforeMute = mainWindow.volumn;
    volumnSlider.value = mainWindow.volumn * 100;
  } else {
    mainWindow.volumn = 1;
    mainWindow.beforeMute = 1;
    volumnSlider.value = 100;
  }
  mainWindow.volumnChange(mainWindow.volumn);
  videoArea.setVolume(mainWindow.volumn);
}

/**
 * 减少音量
 * 每次减少 0.1（即 10%），最小为 0（静音）
 * 同时更新静音前的音量记录（beforeMute）
 */
function volumnDown() {
  if (mainWindow.volumn < 0.1) {
    mainWindow.volumn = 0;
    mainWindow.beforeMute = 0;
    volumnSlider.value = 0;
  } else {
    mainWindow.volumn = mainWindow.volumn - 0.1;
    mainWindow.beforeMute = mainWindow.volumn;
    volumnSlider.value = mainWindow.volumn * 100;
  }
  mainWindow.volumnChange(mainWindow.volumn);
  videoArea.setVolume(mainWindow.volumn);
}

/**
 * 音量滑块拖动响应
 * 根据滑块的值（0-100）换算为音量（0.0-1.0），同步更新音量和静音前记录
 */
function volumeSliderOnMoved() {
  mainWindow.volumn = volumnSlider.value / 100;
  mainWindow.beforeMute = volumnSlider.value / 100;
  mainWindow.volumnChange(mainWindow.volumn);
  videoArea.setVolume(mainWindow.volumn);
}

/**
 * 扬声器图标点击事件（静音切换）
 * 如果当前为静音状态，则恢复到静音前的音量；
 * 如果当前有音量，则保存当前音量并设为静音
 */
function speakerOnClicked() {
  if (mainWindow.volumn === 0) {
    mainWindow.volumn = mainWindow.beforeMute;
    volumnSlider.value = Math.floor(mainWindow.volumn * 100);
  } else {
    mainWindow.beforeMute = mainWindow.volumn;
    mainWindow.volumn = 0;
    volumnSlider.value = 0;
  }
  mainWindow.volumnChange(mainWindow.volumn);
  videoArea.setVolume(mainWindow.volumn);
}

/**
 * 播放模式切换
 * 在三种模式之间循环切换：ordered（顺序播放）→ single（单曲循环）→ random（随机播放）
 */
function playModeOnClicked() {
  if (mainWindow.playState === "ordered") {
    mainWindow.playState = "single";
  } else if (mainWindow.playState === "single") {
    mainWindow.playState = "random";
  } else {
    mainWindow.playState = "ordered";
  }
  mainWindow.playModeChange(playState);
}

/**
 * 倒放切换
 * 切换视频的正向/反向播放方向
 */
function invertedOnClicked() {
  if (mainWindow.isInverted) {
    mainWindow.isInverted = false;
    videoArea.forward();
  } else {
    mainWindow.isInverted = true;
    videoArea.backward();
  }
  //mainWindow.inverted(mainWindow.step)
}

/**
 * 文件列表面板显示/隐藏切换
 */
function fileListOnClicked() {
  if (mainWindow.isVideoListOpen) {
    mainWindow.isVideoListOpen = false;
  } else {
    mainWindow.isVideoListOpen = true;
  }
}

/**
 * 格式化视频进度时间显示
 * @param {boolean} flag - true 表示显示当前时间，false 表示显示剩余时间
 * @returns {string} 格式化后的时间字符串
 *   - 不足 60 秒：直接返回秒数
 *   - 60 秒 ~ 1 小时：返回 "分:秒" 格式
 *   - 超过 1 小时：返回 "时:分:秒" 格式
 */
function videoSlideDistance(flag) {
  let tmp;
  if (flag) {
    tmp = Math.round(mainWindow.currentTime);
  } else {
    tmp = Math.round(mainWindow.endTime - mainWindow.currentTime);
  }
  if (tmp < 60) {
    return tmp + "";
  } else if (tmp >= 60 && tmp < 3600) {
    let tal = tmp % 60;
    let mid = Math.round(tmp / 60);
    if (tal < 10) {
      tal = "0" + tal;
    }
    return mid + ":" + tal;
  } else {
    let tal = tmp % 60;
    let had = Math.round(tmp / 3600);
    let mid = Math.round(tmp / 60) % 60;
    if (tal < 10) {
      tal = "0" + tal;
    }
    if (mid < 10) {
      mid = "0" + mid;
    }
    return had + ":" + mid + ":" + tal;
  }
}

/**
 * 视频区域点击事件（播放/暂停切换）
 * 点击视频画面时切换播放和暂停状态
 */
function videoAreaOnClicked() {
  if (mainWindow.isPlay) {
    mainWindow.isPlay = false;
    mainWindow.stop();
  } else {
    mainWindow.isPlay = true;
    mainWindow.start();
  }
}

/**
 * 主区域初始化
 * 连接 mainWindow 的信号与 videoArea 的对应槽函数：
 * - start → 开始播放
 * - stop → 暂停播放
 * - openFile → 打开文件
 * - setSpeed → 设置播放速度
 */
function mainAreaInit() {
  mainWindow.start.connect(videoArea.start);
  mainWindow.stop.connect(videoArea.pause);
  mainWindow.openFile.connect(videoArea.openFile);
  mainWindow.setSpeed.connect(videoArea.setSpeed);
}

/**
 * 检查视频是否到达边界
 * 倒放时检查是否到达开头（左边界），正放时检查是否到达结尾（右边界）
 * @returns {boolean} true 表示已到达边界，false 表示未到达
 */
function isBoundary() {
  // 左边界：倒放时到达视频开头
  if (mainWindow.isInverted && mainWindow.currentTime <= 0) {
    toVideoBegining();
    operationFailedDialogText.text = "已到达开头，无法继续倒放";
    operationFailedWindow.show();
    return true;
  }
  // 右边界：正放时到达视频结尾
  else if (
    !mainWindow.isInverted &&
    mainWindow.currentTime >= mainWindow.endTime
  ) {
    toVideoEnd();
    nextOnClicked();
    toVideoBegining();
    return true;
  }
  return false;
}

/**
 * 定时器触发回调
 * 每帧更新当前播放时间，检查边界，并触发歌词更新
 */
function timerOnTriggered() {
  mainWindow.currentTime = videoArea.getPTS();
  if (!isBoundary()) {
    videoSlide.value = mainWindow.currentTime;
  }
  triggerLyricUpdate();
}

/**
 * 跳转到视频开头
 * 停止播放，将当前时间重置为 0，并唤醒滑块更新
 */
function toVideoBegining() {
  mainWindow.isPlay = false;
  mainWindow.currentTime = 0;
  mainWindow.wakeSlide();
}

/**
 * 跳转到视频结尾
 * 停止播放，将当前时间设为视频总时长，并更新进度条
 */
function toVideoEnd() {
  mainWindow.isPlay = false;
  mainWindow.currentTime = mainWindow.endTime;
  videoSlide.value = mainWindow.endTime;
}

/**
 * 暂停并重置到开头
 * 停止播放、终止定时器，并将进度条归零
 */
function toPause() {
  toVideoBegining();
  mainWindow.cease();
  mainWindow.stop();
  videoArea.seek(0);
}

/**
 * 播放/暂停功能
 * 如果当前未播放且未到达边界，则开始播放；
 * 如果当前正在播放，则暂停
 */
function playOrPauseFunction() {
  if (!mainWindow.isPlay) {
    if (mainWindow.endTime !== 0.0 && !isBoundary()) {
      mainWindow.isPlay = true;
      mainWindow.start();
    }
  } else {
    mainWindow.isPlay = false;
    mainWindow.stop();
  }
}

/**
 * 视频状态变化处理
 * 根据 videoArea.state 的不同值执行对应操作：
 * - 1: 跳转到开头
 * - 2: 重置结束时间为 0 并跳转到开头
 * - 4: 标记为播放中
 * - 6: 标记为暂停
 */
function solveStateChanged() {
  if (videoArea.state == 1) {
    toVideoBegining();
  } else if (videoArea.state == 2) {
    mainWindow.endTime = 0;
    toVideoBegining();
    return;
  } else if (videoArea.state == 4) {
    mainWindow.isPlay = true;
  } else if (videoArea.state == 6) {
    mainWindow.isPlay = false;
  }
}

/**
 * 切换到下一个视频
 * 根据播放模式选择下一个视频：
 * - ordered: 顺序切换到列表中的下一个
 * - random: 随机选择一个视频
 * - single: 不切换（保持当前视频循环）
 */
function nextOnClicked() {
  console.log("playState:", mainWindow.playState);
  if (mainWindow.playState === "ordered")
    listview.currentIndex = (listview.currentIndex + 1) % listview.count;
  else if (mainWindow.playState === "random")
    listview.currentIndex =
      (listview.currentIndex + Math.floor(Math.random() * listview.count)) %
      listview.count;
  else;
  console.log("index:", listview.currentIndex);
  mainWindow.openFile(listModel.get(listview.currentIndex).filePath);
  mainWindow.endTime = Math.floor(videoArea.getVideoDuration());
}

/**
 * 创建音频输出设备菜单
 * 销毁旧菜单（如果存在），根据传入的设备列表动态创建新的设备选择菜单
 * @param {string[]} list - 可用音频输出设备名称列表
 */
function makeDeviceMenu(list) {
  if (mainWindow.devicesMenuStation) {
    mainWindow.devicesMenuStation.destroy();
  }
  mainWindow.devicesMenuStation = Qt.createQmlObject(
    "import QtQuick 2.13; import QtQuick.Controls 2.13; Menu{}",
    menu
  );
  menu.addItem(mainWindow.devicesMenuStation);
  let component = Qt.createComponent("OutputDevice.qml");
  for (let i = 0; i < list.length; i++) {
    let item = component.createObject(mainWindow.devicesMenuStation, {
      text: list[i],
      deviceName: list[i],
    });
    item.selectDevice.connect(videoArea.setSelectedAudioOutputDevice);
    devicesMenu.addItem(item);
  }
}

/**
 * 创建音频轨道菜单
 * 获取视频中可用的音频轨道列表，动态创建轨道选择菜单项
 */
function makeTrackMenu() {
  if (mainWindow.trackMenu) {
    mainWindow.trackMenu.destroy();
  }
  var tmpList = videoArea.getTracks();
  mainWindow.audioTrack = tmpList[0]
  mainWindow.trackMenu = Qt.createQmlObject(
    "import QtQuick 2.13; import QtQuick.Controls 2.13; Menu{}",
    menu
  );
  menu.addItem(mainWindow.trackMenu);
  let component = Qt.createComponent("TrackItem.qml");
  for (let i = 0; i < tmpList.length; i++) {
    let item = component.createObject(mainWindow.trackMenu, {
      trackID: i,
      trackName: tmpList[i]
    });
    item.setTrack.connect(videoArea.setTrack);
    trackmenu.addItem(item);
  }
}

/**
 * 创建最近播放文件菜单
 * 从 mediaLibController 获取最近播放的文件列表，动态创建菜单项
 */
function makeFileList() {
  if (mainWindow.currentFilePathStation) {
    mainWindow.currentFilePathStation.destroy();
  }
  var tmpList = mediaLibController.getRecentFiles();
  mainWindow.currentFilePathStation = Qt.createQmlObject(
    "import QtQuick 2.13; import QtQuick.Controls 2.13; Menu{}",
    menu
  );
  menu.addItem(mainWindow.currentFilePathStation);
  let component = Qt.createComponent("CurrentFileItem.qml");
  for (let i = 0; i < tmpList.length; i++) {
    let item = component.createObject(mainWindow.currentFilePathStation, {
      text: tmpList[i][0],
      filePath: tmpList[i][1],
      fileName: tmpList[i][0],
    });
    item.addFilePath.connect(videoListOperatorOnAccepted);
    currentFilePathList.addItem(item);
  }
}

/**
 * 处理文件选择操作
 * 当用户通过文件对话框或菜单选择一个视频文件时触发。
 * 更新最近播放列表，打开文件，尝试加载歌词，并将文件添加到播放列表。
 * @param {string} [path=""] - 文件路径（从菜单选择时传入）
 * @param {string} [name=""] - 文件名（从菜单选择时传入）
 */
function videoListOperatorOnAccepted(path = "", name = "") {
  // 读取文件打开对话框的文件名和当前文件夹路径
  let acceptedFileName = fileDialog.currentFile;
  let acceptedFileFold = fileDialog.currentFolder;
  if (path != "") {
    acceptedFileName = path;
    let folder = path.replace(name, "");
    folder = folder.substring(0, folder.length - 1);
    acceptedFileFold = folder;
  }
  // 更新最近播放记录
  mediaLibController.updateRecentFile(acceptedFileName);
  mainWindow.openFile(acceptedFileName);
  wave.waveArea.tryLoadLyrics(acceptedFileName);
  mainWindow.endTime = Math.floor(videoArea.getVideoDuration());
  // 检查文件是否已在播放列表中
  var exists = false;
  for (var i = 0; i < listModel.count; i++) {
    if (listModel.get(i).filePath == acceptedFileName) {
      listview.currentIndex = i;
      exists = true;
      break;
    }
  }
  // 如果不在播放列表中，则添加
  if (!exists) {
    let selectedFileName = acceptedFileName
      .toString()
      .substring(acceptedFileFold.toString().length + 1);
    var getIconPath = mediaLibController.getFile(
      selectedFileName,
      acceptedFileName
    );
    if (getIconPath == "") {
      getIconPath = "interfacepics/defaultlogo";
    }
    listModel.append({
      fileName: selectedFileName,
      filePath: acceptedFileName.toString(),
      iconPath: getIconPath,
    });

    listview.currentIndex = listModel.count - 1;
  }
}

/**
 * 从完整路径中提取文件夹路径（调试用）
 * @param {string} path - 完整文件路径
 * @param {string} name - 文件名
 */
function trans(path, name) {
  let folder = path.replace(name, "");
  console.log(folder);
}

/**
 * 隐藏界面组件（进入简洁模式）
 * 隐藏视频列表、底部栏、顶部栏，并启用鼠标标志
 */
function hideComponents() {
  mainWindow.isVideoListOpen = false;
  mainWindow.isFooterVisible = false;
  mainWindow.isTopBarVisible = false;
  mainWindow.mouseFlag = true;
}

/**
 * 显示界面组件（退出简洁模式）
 * 重新启动自动隐藏定时器，显示底部栏和顶部栏
 */
function showComponents() {
  holder.restart();
  mainWindow.isFooterVisible = true;
  mainWindow.isTopBarVisible = true;
}

/**
 * 窗口最大化/还原切换
 * 在全屏模式不可用时，切换窗口的最大化和正常状态
 */
function screenSizeFunction() {
  mainWindow.isFullScreen = false;
  if (mainWindow.visibility === 2) {
    mainWindow.visibility = 4;
    mainWindowReduction.imageSource = "interfacepics/mainWindowReduction";
  } else {
    mainWindow.visibility = 2;
    mainWindowReduction.imageSource = "interfacepics/mainWindowMaximize";
  }
}

/**
 * 全屏模式切换（底部栏触发）
 * 切换窗口的全屏和正常显示状态，同时控制界面组件的显示/隐藏
 */
function footerScreenSizeFunction() {
  if (mainWindow.isFullScreen) {
    mainWindow.showNormal();
    showComponents();
    mainWindow.isFullScreen = false;
  } else {
    mainWindow.showFullScreen();
    mainWindow.isFullScreen = true;
  }
}

/**
 * 底部栏组件加载完成后的初始化
 * 连接信号：
 * - wakeSlide → 重置进度条到开头
 * - mainWindowLostFocus → 处理窗口失去焦点
 */
function footerOnCompleted() {
  mainWindow.wakeSlide.connect(sliderToFront);
  mainWindow.mainWindowLostFocus.connect(lostFocus);
}

/**
 * 将视频进度条滑块重置到开头（0.0）
 */
function sliderToFront() {
  videoSlide.value = 0.0;
}

/**
 * 窗口失去焦点时的处理
 * 隐藏视频预览缩略图
 */
function lostFocus() {
  previewRect.visible = false;
}

/**
 * 触发歌词更新
 * 根据当前播放时间查找对应的歌词句子，并滚动歌词区域到当前位置
 */
function triggerLyricUpdate() {
  var currentLyricIndex = 0;
  for (
    ;
    currentLyricIndex < wave.lyricsData.sentences.length;
    currentLyricIndex++
  ) {
    if (
      wave.lyricsData.sentences[currentLyricIndex].startTime <
        mainWindow.currentTime &&
      wave.lyricsData.sentences[currentLyricIndex].endTime >
        mainWindow.currentTime
    )
      break;
  }
  if (wave.lyricsData.sentences.length) {
    wave.lyricsArea.flick.contentY =
      wave.lyricsArea.rep.itemAt(currentLyricIndex).y -
      wave.lyricsArea.height / 2;
    wave.lyricsArea.flick.currentIndex = currentLyricIndex;
  }
}

var dbusComponent;
var dbusWidget;

/**
 * 主窗口初始化
 * 检测操作系统平台：在 macOS（osx）上加载 DBus 组件以适配系统菜单栏
 */
function mainWindowInit() {
  console.log("main window init found os: " + Qt.platform.os);
  if (Qt.platform.os === "osx") {
    dbusComponent = Qt.createComponent("DBus.qml");
    if (dbusComponent.status === Component.Ready) {
      dbusWidget = dbusComponent.createObject(mainWindow, { id: dbus });
      topBar.height = 600;
      topBar.visible = false;
    }
  }
}

/**
 * 判断序列化状态并恢复播放
 * 当应用从序列化状态恢复时，如果之前正在播放视频，则自动继续播放
 */
function judgeSerialize(){
  console.log("[serialize]",mainWindow.serialize);
  if(mainWindow.serialize){
    if (mainWindow.endTime !== 0.0 && !isBoundary()) {
      mainWindow.isPlay = true;
      mainWindow.start();
    }
  }
}
