#include <faset/render/renderer.hpp>
#include <faset/render/render_graph.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace faset::render;
void require(bool test,const char* message){if(!test)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc>1&&std::string(argv[1])=="--unit"){
        int count{};RenderGraph invalid;invalid.add("consumer",{"missing"},{},[&]{++count;});bool caught{};try{invalid.execute();}catch(const std::runtime_error&){caught=true;}require(caught&&count==0,"Graph must validate before side effects");RenderGraph graph;graph.import("external");graph.add("first",{"external"},{"color"},[&]{require(count==0,"Pass order");++count;});graph.add("second",{"color"},{},[&]{++count;});graph.execute();require(count==2,"Pass execution count");auto t=transform({2,3,4},{},{2,3,4});require(t[12]==2&&t[13]==3&&t[14]==4,"Transform translation");auto m=multiply(identity,t);require(m==t,"Matrix multiplication identity");require(cube_mesh()->indices.size()==36,"Cube triangle topology");std::cout<<"Render graph and math contracts passed\n";return 0;
    }
    bool visible=argc>1&&std::string(argv[1])=="--visible";
    Renderer renderer({320,240,"Faset render validation",!visible,true});
    Snapshot scene;scene.eye={4,3,5};scene.view_projection=multiply(perspective(.85f,320.f/240.f,.1f,100),look_at(scene.eye,{0,0,0}));scene.draws.push_back({cube_mesh(),transform({0,0,0}),{.2f,.65f,.95f,1},.4f,.15f,true});scene.draws.push_back({cube_mesh(),transform({0,-1,0},{},{8,.2f,8}),{.45f,.48f,.5f,1},.8f,0,true});scene.ui_quads.push_back({8,8,70,16,{.8f,.1f,.15f,1}});
    auto texture=std::make_shared<Texture>();texture->width=texture->height=1;texture->rgba={20,220,40,255};scene.ui_quads.push_back({260,8,40,20,{1,1,1,1},texture});
    renderer.render(scene);require(renderer.stats().validation_errors==0,"Vulkan validation reported an error");auto pixels=renderer.pixels();require(pixels.size()==320*240*4,"Readback dimensions");auto index=(10*320+10)*4;require(pixels[index]>190&&pixels[index+1]<50,"Colored UI pixel");index=(10*320+270)*4;require(pixels[index]<30&&pixels[index+1]>200,"Textured UI pixel");
    auto shadowed=pixels;
    if(argc>2)renderer.capture(std::string(argv[2])+".shadowed.ppm");
    for(auto& draw:scene.draws)draw.cast_shadow=false;
    renderer.render(scene);pixels=renderer.pixels();std::size_t shadow_difference{};for(std::size_t i=0;i<pixels.size();i+=4)if(pixels[i]>shadowed[i]+8)++shadow_difference;if(shadow_difference<=20){std::cerr<<"Shadow difference pixels: "<<shadow_difference<<"\n";if(argc>2)renderer.capture(std::string(argv[2])+".unshadowed.ppm");}require(shadow_difference>20,"Directional shadow must darken rendered surface pixels");
    for(auto& draw:scene.draws)draw.cast_shadow=true;
    std::string reload_error;require(renderer.reload_shaders(reload_error),"Compatible shader pipeline reload");
    texture->rgba={40,30,230,255};++texture->revision;renderer.render(scene);pixels=renderer.pixels();require(pixels[index+2]>220,"Texture revision upload");
    if(argc>2)renderer.capture(argv[2]);renderer.resize(400,300);renderer.poll_events();renderer.render(scene);require(renderer.width()==400&&renderer.height()==300,"Render target resize");require(renderer.stats().validation_errors==0,"Resize validation error");
    std::cout<<"Vulkan frame, shadow/PBR, atlas upload, readback and resize passed on "<<renderer.stats().device<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}return 0;}
