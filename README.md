# ChineseChess
尝试使用vulkan渲染一个中国象棋的棋盘和棋子。

使用C++的module编写。

使用vcpkg进行包管理，索引获取网站使用gitee的镜像站加速访问，包下载仍然从github下载。

如果缺少vulkan和glm对应的module，需要手动将vcpkg安装的包的目录下的vulkan.cppm和glm.cppm添加到项目中。

module不支持宏定义，宏定义在项目属性中管理。

字体使用[LXGW WenKai GB / 霞鹜文楷 GB](https://github.com/lxgw/LxgwWenkaiGB)。