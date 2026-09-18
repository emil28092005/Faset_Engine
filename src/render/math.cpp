#include <faset/render/renderer.hpp>
#include <cmath>
#include <stdexcept>
namespace faset::render {
namespace {
Vec3 sub(Vec3 a, Vec3 b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
float dot(Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec3 cross(Vec3 a,Vec3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Vec3 unit(Vec3 a){float l=std::sqrt(dot(a,a)); if(l<1e-6f) throw std::invalid_argument("Degenerate camera axis"); return {a[0]/l,a[1]/l,a[2]/l};}
}
Mat4 multiply(const Mat4& a,const Mat4& b){Mat4 r{};for(int c=0;c<4;++c)for(int y=0;y<4;++y)for(int k=0;k<4;++k)r[c*4+y]+=a[k*4+y]*b[c*4+k];return r;}
Mat4 transform(Vec3 p,Vec3 r,Vec3 s){
    const float cx=std::cos(r[0]),sx=std::sin(r[0]),cy=std::cos(r[1]),sy=std::sin(r[1]),cz=std::cos(r[2]),sz=std::sin(r[2]);
    Mat4 x{1,0,0,0,0,cx,sx,0,0,-sx,cx,0,0,0,0,1};
    Mat4 y{cy,0,-sy,0,0,1,0,0,sy,0,cy,0,0,0,0,1};
    Mat4 z{cz,sz,0,0,-sz,cz,0,0,0,0,1,0,0,0,0,1};
    auto m=multiply(z,multiply(y,x));for(int c=0;c<3;++c)for(int i=0;i<3;++i)m[c*4+i]*=s[c];m[12]=p[0];m[13]=p[1];m[14]=p[2];return m;
}
Mat4 perspective(float fov,float aspect,float n,float f){if(aspect<=0||n<=0||f<=n)throw std::invalid_argument("Invalid perspective volume");float q=1/std::tan(fov*.5f);return {q/aspect,0,0,0,0,-q,0,0,0,0,f/(n-f),-1,0,0,n*f/(n-f),0};}
Mat4 orthographic(float l,float r,float b,float t,float n,float f){if(r==l||t==b||f==n)throw std::invalid_argument("Invalid orthographic volume");return {2/(r-l),0,0,0,0,-2/(t-b),0,0,0,0,1/(n-f),0,-(r+l)/(r-l),(t+b)/(t-b),n/(n-f),1};}
Mat4 look_at(Vec3 e,Vec3 t,Vec3 up){auto f=unit(sub(t,e));auto s=unit(cross(f,up));auto u=cross(s,f);return {s[0],u[0],-f[0],0,s[1],u[1],-f[1],0,s[2],u[2],-f[2],0,-dot(s,e),-dot(u,e),dot(f,e),1};}
std::shared_ptr<const Mesh> cube_mesh(){static auto mesh=[](){auto m=std::make_shared<Mesh>();
 const Vec3 normals[]={{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
 const Vec3 points[][4]={{{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}},{{.5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{.5f,.5f,-.5f}},{{.5f,-.5f,.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f}},{{-.5f,-.5f,-.5f},{-.5f,-.5f,.5f},{-.5f,.5f,.5f},{-.5f,.5f,-.5f}},{{-.5f,.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f}},{{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{-.5f,-.5f,.5f}}};
 for(int face=0;face<6;++face){for(auto p:points[face])m->vertices.push_back({p,normals[face],{1,1,1,1}});for(auto i:{0u,1u,2u,0u,2u,3u})m->indices.push_back(face*4+i);}return m;}();return mesh;}
}
