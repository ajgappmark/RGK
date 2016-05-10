#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/cimport.h>

#include <iostream>
#include <iomanip>
#include <queue>
#include <cmath>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sstream>

#include "../external/ctpl_stl.h"

#include "LRU.hpp"

#include "scene.hpp"
#include "ray.hpp"
#include "texture.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "texture.hpp"
#include "camera.hpp"

static bool debug_trace = false;
static unsigned int debug_x, debug_y;

#define SHADOW_CACHE_SIZE 5

Color trace_ray(const Scene& scene, const Ray& r, const std::vector<Light>& lights, std::vector<LRUBuffer<const Triangle*>>& shadow_cache, const Config& cfg, int depth, unsigned int& raycount, bool debug = false){
    if(debug) std::cerr << "Debugging a ray. " << std::endl;
    if(debug) std::cerr << r.origin << " " << r.direction << std::endl;
    raycount++;
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

        if(mat.bump_texture){
            //diffuse = Color(0.5, 0.5, 0.5);

            float right = mat.bump_texture->GetSlopeRight(texUV);
            float bottom = mat.bump_texture->GetSlopeBottom(texUV);
            glm::vec3 tangent = i.Interpolate(i.triangle->GetTangentA(),
                                              i.triangle->GetTangentB(),
                                              i.triangle->GetTangentC());
            glm::vec3 bitangent = glm::normalize(glm::cross(N,tangent));

            N = glm::normalize(N + (tangent*right + bitangent*bottom) * cfg.bumpmap_scale);
        }

        if(debug) std::cerr << "Was hit. color is " << diffuse << std::endl;

        for(unsigned int qq = 0; qq < lights.size(); qq++){
            const Light& l = lights[qq];
            glm::vec3 L = glm::normalize(l.pos - ipos);
            const Triangle* shadow_triangle = nullptr;
            if(depth > 0){
                // Search for shadow triangle.
                Ray ray_to_light(ipos, l.pos, scene.epsilon * 2.0f * glm::length(ipos - l.pos));
                // First, try looking within shadow cache.
                if(debug) std::cout << "raytolight origin:" << ray_to_light.origin << ", dir" << ray_to_light.direction << std::endl;
                for(const Triangle* tri : shadow_cache[qq]){
                    float t,a,b;
                    raycount++;
                    if(tri->TestIntersection(ray_to_light, t, a, b, debug)){
                        if(t < ray_to_light.near - scene.epsilon || t > ray_to_light.far + scene.epsilon) continue;
                        // Intersection found.
                        if(debug) std::cout << "Shadow found in cache at " << tri << "." << std::endl;
                        if(debug) std::cout << "Triangle " << tri->GetVertexA() << std::endl;
                        if(debug) std::cout << "Triangle " << tri->GetVertexB() << std::endl;
                        if(debug) std::cout << "Triangle " << tri->GetVertexC() << std::endl;
                        if(debug) std::cout << "t " << t << std::endl;
                        shadow_triangle = tri;
                        break;
                    }
                }
                // Skip manual search when a shadow triangle was found in cache
                if(!shadow_triangle){
                    raycount++;
                    shadow_triangle = scene.FindIntersectKdAny(ray_to_light);
                }
            }
            if(!shadow_triangle){ // no intersection found
                //TODO: use interpolated normals

                float distance = glm::length(ipos - l.pos); // The distance to light
                if(debug) std::cout << "Distance to light : " << distance << std::endl;
                float d = distance * distance;

                /*
                float dist_func = 1.0f/(2.5f + d); // Light intensity falloff function
                if(debug) std::cout << "Dist func : " << dist_func << std::endl;
                float intens_factor = l.intensity * 0.18f * dist_func;
                */

                float dist_func = 1.0f/(3.0f + d)/4.85f; // Light intensity falloff function
                if(debug) std::cout << "Dist func : " << dist_func << std::endl;
                float intens_factor = l.intensity * 1.0f * dist_func;

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
                    if(debug) std::cerr << "a: " << a << std::endl;
                    if(debug) std::cerr << "specular: " << specular << std::endl;
                    float kS = glm::pow(a, mat.exponent);
                    // if(std::isnan(kS)) std::cout << glm::dot(R,V) << "/" << mat.exponent << std::endl;
                    if(debug) std::cerr << "spec add: " << intens_factor * l.color * specular * kS * 1.0f << std::endl;
                    total += intens_factor * l.color * specular * kS * 1.0f;
                }

            }else{
                if(debug) std::cerr << "Shadow found." << std::endl;
                // Update the shadow buffer for this light source
                shadow_cache[qq].Use(shadow_triangle);
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
            Color reflection = trace_ray(scene, refl_ray, lights, shadow_cache, cfg, depth-1, raycount);
            total = mat.exponent * reflection + (1.0f - mat.exponent) * total;
        }
        if(debug) std::cout << "Total: " << total << std::endl;
        return total;
    }else{
        // Black background for void spaces
        return cfg.sky_color;
    }
}

struct RenderTask{
    RenderTask(unsigned int xres, unsigned int yres, unsigned int x1, unsigned int x2, unsigned int y1, unsigned int y2)
        : xres(xres), yres(yres), xrange_start(x1), xrange_end(x2), yrange_start(y1), yrange_end(y2)
    {
        glm::vec2((xrange_start + xrange_end)/2.0f, (yrange_start + yrange_end)/2.0f);
    }
    unsigned int xres, yres;
    unsigned int xrange_start, xrange_end;
    unsigned int yrange_start, yrange_end;
    glm::vec2 midpoint;
};

std::atomic<int> tasks_done(0);
std::atomic<int> pixels_done(0);
std::atomic<unsigned int> raycount(0);
std::atomic<bool> stop_monitor(false);
int total_pixels;

void Render(RenderTask task, const Scene& scene, const Camera& camera, const std::vector<Light>& lights, const Config& config, Texture* output){
    unsigned int pxdone = 0, raysdone = 0;
    unsigned int m = config.multisample;
    // Per-thread shadow cache
    std::vector<LRUBuffer<const Triangle*>> shadow_cache(lights.size(), LRUBuffer<const Triangle*>(SHADOW_CACHE_SIZE));

    for(unsigned int y = task.yrange_start; y < task.yrange_end; y++){
        for(unsigned int x = task.xrange_start; x < task.xrange_end; x++){
            bool d = false;
            if(debug_trace && x == debug_x && y == debug_y) d = true;
            Color pixel_total(0.0, 0.0, 0.0);
            for(unsigned int my = 0; my < m; my++){
                for(unsigned int mx = 0; mx < m; mx++){
                    Ray r;
                    unsigned int mx2 = (my % 2 == 0) ? (m - mx -1) : mx;
                    if(camera.IsSimple()){
                        r = camera.GetSubpixelRay(x, y, task.xres, task.yres, mx2, my, m);
                    }else{
                        r = camera.GetRandomRayLens(x, y, task.xres, task.yres);
                        //r = camera.GetSubpixelRayLens(x, y, task.xres, task.yres, mx2, my, m);
                    }
                    pixel_total += trace_ray(scene, r, lights, shadow_cache, config, config.recursion_level, raysdone, d);
                }
            }
            output->SetPixel(x, y, pixel_total * (1.0f / (m*m)));
            pxdone++;
            if(pxdone % 100 == 0){
                pixels_done += 100;
                pxdone = 0;
            }
        }
    }
    pixels_done += pxdone;
    raycount += raysdone;
    tasks_done++;
}

std::string float_to_percent_string(float f){
    std::stringstream ss;
    ss << std::setw(5) << std::fixed << std::setprecision(1) << f << "%";
    return ss.str();
}

void Monitor(const Texture* output_buffer, std::string preview_path){
    std::cout << "Monitor thread started" << std::endl;
    int counter = 0;

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
        std::cout << "] " <<  float_to_percent_string(percent) << " done.";
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

    std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
    float total_seconds = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f;
    unsigned int total_rays = raycount;

    std::cout << "Total rendering time: " << total_seconds << "s" << std::endl;
    std::cout << "Total pixels: " << total_pixels << ", total rays: " << total_rays << std::endl;
    std::cout << "Average pixels per second: " << Utils::FormatIntThousands(total_pixels / total_seconds) << "." << std::endl;
    std::cout << "Average rays per second: " << Utils::FormatIntThousands(total_rays / total_seconds) << std::endl;

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

    srand(time(nullptr));

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

    std::cout << "Loading scene... " << std::endl;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE, NULL);
    const aiScene* scene = importer.ReadFile(modelfile,
                                             aiProcess_Triangulate |
                                             //aiProcess_TransformUVCoords |
                                             //aiProcess_GenNormals |
                                             aiProcess_GenSmoothNormals |
                                             aiProcess_JoinIdenticalVertices |
                                             aiProcess_RemoveRedundantMaterials |
                                             aiProcess_GenUVCoords |
                                             //aiProcess_SortByPType |
                                             aiProcess_FindDegenerates |
                                             // DO NOT ENABLE THIS CLEARLY BUGGED SEE SIBENIK MODEL aiProcess_FindInvalidData |
                                             //aiProcess_ValidateDataStructure |
                     0 );

    if(!scene){
        std::cout << "Assimp failed to load scene `" << modelfile << "`: " << importer.GetErrorString() << std::endl;
        return 1;
    }

    // Calculating tangents is requested AFTER the scene is
    // loaded. Otherwise this step runs before normals are calculated
    // for vertices that are missing them.
    scene = importer.ApplyPostProcessing(aiProcess_CalcTangentSpace);
    //aiApplyPostProcessing(scene, aiProcess_CalcTangentSpace);

    std::cout << "Loaded scene with " << scene->mNumMeshes << " meshes, " <<
        scene->mNumMaterials << " materials and " << scene->mNumLights <<
        " lights." << std::endl;

    Scene s;
    s.texture_directory = modeldir + "/";
    s.LoadScene(scene);
    s.Commit();

    Camera camera(cfg.view_point,
                  cfg.look_at,
                  cfg.up_vector,
                  cfg.yview,
                  cfg.yview*cfg.xres/cfg.yres,
                  cfg.focus_plane,
                  cfg.lens_size
                  );

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
            RenderTask task(cfg.xres, cfg.yres, xp, std::min(cfg.xres, xp+tile_size),
                                                yp, std::min(cfg.yres, yp+tile_size));
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
        tpool.push( [&, task](int){Render(task, s, camera, cfg.lights, cfg, &ob);} );
    }

    tpool.stop(true); // Waits for all remaining worker threads to complete.

    stop_monitor = true;
    if(monitor_thread.joinable()) monitor_thread.join();

    ob.Write(cfg.output_file);

    return 0;
}