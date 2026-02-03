# ChineseChess
尝试使用vulkan渲染一个中国象棋的棋盘和棋子。

使用C++23编写。

使用vcpkg进行包管理，索引获取网站使用gitee的镜像站加速访问，包下载仍然从github下载。

项目编译中添加了/utf8，宏添加了NOMINMAX。

字体使用[LXGW WenKai GB / 霞鹜文楷 GB](https://github.com/lxgw/LxgwWenkaiGB)。

按C保存当前帧到captures文件夹下。

接入imgui，可以弹窗选择txt的棋谱文件。

棋子渲染是将纹理组合成一个纹理数组提交，然后采样对应的层。ubo也组合成一个巨大的ubo提交。

由于vulkan渲染中无法复制图像，于是让场景和UI分别渲染到自己的图像上，最后使用shader按照UI的alpha通道混合两张图像，这样保存的时候可以仅保存场景没有UI。

早期提交需要看master分支，该分支尝试使用module，但是代码提示太差，还是换回头文件模式。