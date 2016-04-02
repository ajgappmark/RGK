#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <iostream>
#include <iomanip>
#include <queue>
#include <cmath>
#include <thread>
#include <chrono>
#include <unistd.h>

#include "external/ctpl_stl.h"

#include "scene.hpp"
#include "ray.hpp"
#include "texture.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "texture.hpp"

static bool debug_trace = false;
static unsigned int debug_x, debug_y;

Color trace_ray(const Scene& scene, const Ray& r, const std::vector<Light>& lights, Color sky_color, int depth, bool debug = false){
    if(debug) std::cerr << "Debugging a ray. " << std::endl;
    if(debug) std::cerr << r.origin << " " << r.direction << std::endl;
    Intersection i = scene.FindIntersectKd(r, debug);

    if(i.triangle){
        if(debug) std::cerr << "Intersection found. " << std::endl;
        const Material& mat = i.triangle->GetMaterial();
        Color total(0.0, 0.0, 0.0);

        glm::vec3 ipos = r[i.t];
        glm::vec3 N = i.Interpolate(i.triangle->GetNormalA(),
                                    i.triangle->GetNormalB(),
                                    i.triangle->GetNormalC());
        glm::vec3 V = -r.direction; // incoming direction

        glm::vec2 texUV;
        if(mat.ambient_texture || mat.diffuse_texture || mat.specular_texture){
            texUV = i.Interpolate(i.triangle->GetTexCoordsA(),
                                  i.triangle->GetTexCoordsB(),
                                  i.triangle->GetTexCoordsC());
        }

        Color diffuse  =  mat.diffuse_texture ?  mat.diffuse_texture->GetPixelInterpolated(texUV,debug) : mat.diffuse ;
        Color specular = mat.specular_texture ? mat.specular_texture->GetPixelInterpolated(texUV,debug) : mat.specular;
        Color ambient  =  mat.ambient_texture ?  mat.ambient_texture->GetPixelInterpolated(texUV,debug) : mat.ambient ;

        if(debug) std::cerr << "Was hit. color is " << diffuse << std::endl;

        for(const Light& l : lights){
            glm::vec3 L = glm::normalize(l.pos - ipos);
            bool shadow;
            if(depth == 0) shadow = false;
            else{
                // if no intersection on path to light
                Ray ray_to_light(ipos, l.pos, 0.0001f * glm::length(ipos - l.pos));
                shadow = scene.FindIntersectKdBool(ray_to_light);
            }
            if(!shadow){ // no intersection found
                //TODO: use interpolated normals

                float distance = glm::length(ipos - l.pos); // The distance to light
                float d = distance;
                float intens_factor = l.intensity*0.1f/(1.0f + d); // Light intensity falloff function

                if(debug) std::cerr << "No shadow, distance: " << distance << std::endl;

                float kD = glm::dot(N, L);
                kD = glm::max(0.0f, kD);
                total += intens_factor * l.color * diffuse * kD;

                if(debug) std::cerr << "N " << N << std::endl;
                if(debug) std::cerr << "L " << L << std::endl;
                if(debug) std::cerr << "kD " << kD << std::endl;
                if(debug) std::cerr << "Total: " << total << std::endl;

                if(mat.exponent > 1.0f){
                    glm::vec3 R = 2.0f * glm::dot(L, N) * N - L;
                    float a = glm::dot(R, V);
                    a = glm::max(0.0f, a);
                    float kS = glm::pow(a, mat.exponent);
                    // if(std::isnan(kS)) std::cout << glm::dot(R,V) << "/" << mat.exponent << std::endl;
                    total += intens_factor * l.color * specular * kS * 1.0f;
                }
            }else{
                if(debug) std::cerr << "Shadow found." << std::endl;
            }
        }

        // Special case for when there is no light
        if(lights.size() == 0){
            total += diffuse;
        }

        // Ambient lighting
        total += ambient * 0.1;

        // Next ray
        if(depth >= 2 && mat.exponent < 1.0f){
            glm::vec3 refl = 2.0f * glm::dot(V, N) * N - V;
            Ray refl_ray(ipos, ipos + refl, 0.01);
            refl_ray.far = 1000.0f;
            Color reflection = trace_ray(scene, refl_ray, lights, sky_color, depth-1);
            total = mat.exponent * reflection + (1.0f - mat.exponent) * total;
        }
        if(debug) std::cout << "Total: " << total << std::endl;
        return total;
    }else{
        // Black background for void spaces
        return sky_color;
    }
}

struct CameraConfig{
public:
    CameraConfig(glm::vec3 pos, glm::vec3 la, glm::vec3 up, float yview, float xview){
        camerapos = pos;
        lookat = la;
        cameraup = up;

        cameradir = glm::normalize( lookat - camerapos);
        cameraleft = glm::normalize(glm::cross(cameradir, cameraup));
        cameraup = glm::normalize(glm::cross(cameradir,cameraleft));

        viewscreen_x = - xview * cameraleft;
        viewscreen_y =   yview * cameraup;
        viewscreen = camerapos + cameradir - 0.5f * viewscreen_y - 0.5f * viewscreen_x;
    }
    glm::vec3 camerapos;
    glm::vec3 lookat;
    glm::vec3 cameraup;
    glm::vec3 cameradir;
    glm::vec3 cameraleft;

    glm::vec3 viewscreen;
    glm::vec3 viewscreen_x;
    glm::vec3 viewscreen_y;

    glm::vec3 GetViewScreenPoint(int x, int y, int xres, int yres) const {
        const float off = 0.5f;
        glm::vec3 xo = (1.0f/xres) * (x + off) * viewscreen_x;
        glm::vec3 yo = (1.0f/yres) * (y + off) * viewscreen_y;
        return viewscreen + xo + yo;
    };
};

struct RenderTask{
    unsigned int xres, yres;
    unsigned int xrange_start, xrange_end;
    unsigned int yrange_start, yrange_end;
    glm::vec2 midpoint;
    const Scene* scene;
    const CameraConfig* cconfig;
    const std::vector<Light>* lights;
    unsigned int multisample;
    unsigned int recursion_level;
    Color sky_color;
    Texture* output;
};

std::atomic<int> tasks_done(0);
std::atomic<int> pixels_done(0);
std::atomic<bool> stop_monitor(false);
int total_pixels;

void Render(RenderTask task){
    unsigned int pxdone = 0;
    unsigned int m = task.multisample;
    for(unsigned int y = task.yrange_start; y < task.yrange_end; y++){
        for(unsigned int x = task.xrange_start; x < task.xrange_end; x++){
            bool d = false;
            if(debug_trace && x == debug_x && y == debug_y) d = true;
            Color pixel_total(0.0, 0.0, 0.0);
            float factor = 1.0/(m*m);
            for(unsigned int my = 0; my < m; my++){
                for(unsigned int mx = 0; mx < m; mx++){
                    glm::vec3 p = task.cconfig->GetViewScreenPoint(x*m + mx, y*m + my, task.xres*m, task.yres*m);
                    Ray r(task.cconfig->camerapos, p - task.cconfig->camerapos);
                    pixel_total += trace_ray(*task.scene, r, *task.lights, task.sky_color, task.recursion_level, d) * factor;
                }
            }
            task.output->SetPixel(x, y, pixel_total);
            pxdone++;
            if(pxdone % 100 == 0){
                pixels_done += 100;
                pxdone = 0;
            }
        }
    }
    pixels_done += pxdone;
    tasks_done++;
}

void Monitor(const Texture* output_buffer, std::string preview_path){
    std::cout << "Monitor thread started" << std::endl;
    int counter = 0;

    std::streamsize orig_precision = std::cout.precision();
    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

    auto print_progress_f = [](){
        int d = pixels_done;
        float fraction = d/(float)total_pixels;
        float percent = int(fraction*1000.0f + 0.5f) / 10.0f;
        const unsigned int barsize = 60;
        unsigned int fill = fraction * barsize;
        unsigned int empty = barsize - fill;
        std::cout << "\33[2K\rRendered " << std::setw(log10(total_pixels) + 1) << d << "/" << total_pixels << " pixels, [";
        for(unsigned int i = 0; i <  fill; i++) std::cout << "#";
        for(unsigned int i = 0; i < empty; i++) std::cout << "-";
        std::cout << "] " << std::setw(5) << std::fixed << std::setprecision(1) << percent << "% done.";
        std::flush(std::cout);
    };

    while(!stop_monitor){
        print_progress_f();
        if(pixels_done >= total_pixels) break;

        if(counter % 10 == 0){
            // Each second
            output_buffer->Write(preview_path);
        }

        usleep(1000*100); // 100ms
        counter++;
    }

    // Display the message one more time to output "100%"
    print_progress_f();
    std::cout << std::endl;
    output_buffer->Write(preview_path);

    // Clear cout attributes
    std::cout << std::defaultfloat << std::setprecision(orig_precision);

    std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
    std::cout << "Total rendering time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f << "s" << std::endl;

}

int main(int argc, char** argv){

    if(argc < 2){
        std::cout << "No input file, aborting." << std::endl;
        return 0;
    }

    if(argc >= 4){
        debug_trace = true;
        debug_x = std::stoi(argv[2]);
        debug_y = std::stoi(argv[3]);
        std::cout << "Debug mode enabled, will trace pixel " << debug_x << " " << debug_y << std::endl;
    }

    std::string configfile = argv[1];

    Config cfg;
    try{
        cfg = Config::CreateFromFile(configfile);
    }catch(ConfigFileException ex){
        std::cout << "Failed to load config file: " << ex.what() << std::endl;
        return 1;
    }

    Assimp::Importer importer;

    std::string configdir = Utils::GetDir(configfile);
    std::string modelfile = configdir + "/" + cfg.model_file;
    std::string modeldir  = Utils::GetDir(modelfile);
    if(!Utils::GetFileExists(modelfile)){
        std::cout << "Unable to find model file `" << modelfile << "`. " << std::endl;
        return 1;
    }

    const aiScene* scene = importer.ReadFile(modelfile,
                                             aiProcessPreset_TargetRealtime_MaxQuality
                      );

    if(!scene){
        std::cout << "Assimp failed to load scene `" << modelfile << "`. " << std::endl;
        return 1;
    }

    std::cout << "Loaded scene with " << scene->mNumMeshes << " meshes, " <<
        scene->mNumMaterials << " materials and " << scene->mNumLights <<
        " lights." << std::endl;

    Scene s;
    s.texture_directory = modeldir + "/";
    s.LoadScene(scene);
    s.Commit();

    CameraConfig cconfig(cfg.view_point, cfg.look_at, cfg.up_vector, cfg.yview, cfg.yview*cfg.xres/cfg.yres);

    Texture ob(cfg.xres, cfg.yres);
    ob.FillStripes(15, Color(0.6,0.6,0.6), Color(0.5,0.5,0.5));

    unsigned int concurrency = std::thread::hardware_concurrency();
    concurrency = std::max((unsigned int)1, concurrency - 1); // If available, leave one core free.
    ctpl::thread_pool tpool(concurrency);

    std::cout << "Using thread pool of size " << concurrency << std::endl;

    if(cfg.recursion_level == 0){
        cfg.lights.clear();
    }

    std::vector<RenderTask> tasks;

    total_pixels = cfg.xres * cfg.yres;

    auto p = Utils::GetFileExtension(cfg.output_file);
    std::thread monitor_thread(Monitor, &ob, p.first + ".preview." + p.second);

    const int tile_size = 200;
    // Split rendering into smaller (tile_size x tile_size) tasks.
    for(unsigned int yp = 0; yp < cfg.yres; yp += tile_size){
        for(unsigned int xp = 0; xp < cfg.xres; xp += tile_size){
            RenderTask task;
            task.xres = cfg.xres; task.yres = cfg.yres;
            task.xrange_start = xp; task.xrange_end = std::min(cfg.xres, xp+tile_size);
            task.yrange_start = yp; task.yrange_end = std::min(cfg.yres, yp+tile_size);
            task.midpoint = glm::vec2((task.xrange_start + task.xrange_end)/2.0f, (task.yrange_start + task.yrange_end)/2.0f);
            task.scene = &s;
            task.cconfig = &cconfig;
            task.lights = &cfg.lights;
            task.multisample = cfg.multisample;
            task.recursion_level = cfg.recursion_level;
            task.sky_color = cfg.sky_color;
            task.output = &ob;
            tasks.push_back(task);
        }
    }

    std::cout << "Rendering in " << tasks.size() << " tiles." << std::endl;

    // Sorting tasks by their distance to the middle
    glm::vec2 middle(cfg.xres/2.0f, cfg.yres/2.0f);
    std::sort(tasks.begin(), tasks.end(), [&middle](const RenderTask& a, const RenderTask& b){
            return glm::length(middle - a.midpoint) < glm::length(middle - b.midpoint);
        });

    for(const RenderTask& task : tasks){
        tpool.push( [task](int){Render(task);} );
    }

    tpool.stop(true); // Waits for all remaining worker threads to complete.

    stop_monitor = true;
    if(monitor_thread.joinable()) monitor_thread.join();

    ob.Write(cfg.output_file);

    return 0;
}
