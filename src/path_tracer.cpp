#include "path_tracer.hpp"

#include "camera.hpp"
#include "scene.hpp"
#include "global_config.hpp"
#include "random_utils.hpp"

#include <tuple>

#include "glm.hpp"
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtc/constants.hpp>

PathTracer::PathTracer(const Scene& scene,
                       const Camera& camera,
                       unsigned int xres,
                       unsigned int yres,
                       unsigned int multisample,
                       unsigned int depth,
                       float clamp,
                       float russian,
                       float bumpmap_scale,
                       bool force_fresnell,
                       unsigned int reverse,
                       Sampler& sampler)
: Tracer(scene, camera, xres, yres, multisample, bumpmap_scale),
  clamp(clamp),
  russian(russian),
  depth(depth),
  force_fresnell(force_fresnell),
  reverse(reverse),
  sampler(sampler)
{
}

PixelRenderResult PathTracer::RenderPixel(int x, int y, unsigned int & raycount, bool debug){
    PixelRenderResult total;

    IFDEBUG std::cout << std::endl;

    for(unsigned int i = 0; i < multisample; i++){

        sampler.Advance();

        glm::vec2 coords = sampler.Get2D();
        Ray r = camera.IsSimple() ?
            camera.GetPixelRay(x, y, xres, yres, coords) :
            camera.GetPixelRayLens(x, y, xres, yres, coords, sampler.Get2D());

        PixelRenderResult q = TracePath(r, raycount, debug);
        total.main_pixel += q.main_pixel;

        for(const auto& p : q.side_effects){
            total.side_effects.push_back(p);
        }

        IFDEBUG std::cout << "Side effects: " << q.side_effects.size() << std::endl;

        IFDEBUG std::cout << "Sampler samples used for this ray: " << sampler.GetUsage() << std::endl;
    }

    IFDEBUG std::cout << "-----> pixel average: " << total.main_pixel/multisample << std::endl;

    return total;
}


Radiance PathTracer::ApplyThinglass(Radiance input, const ThinglassIsections& isections, glm::vec3 ray_direction) const {
    Radiance result = input;
    float ct = -1.0f;
    for(int n = isections.size()-1; n >= 0; n--){
        const Triangle* trig = isections[n].first;
        // Ignore repeated triangles within epsillon radius from
        // previous thinglass - they are probably clones of the same
        // triangle in kd-tree.
        float newt = isections[n].second;
        if(newt <= ct + scene.epsilon) continue;
        ct = newt;
        // This is just to check triangle orientation, so that we only
        // apply color filter when the ray is entering glass.
        glm::vec3 N = trig->generic_normal();
        if(glm::dot(N,ray_direction) >= 0){
            // TODO: Use translucency filter instead of diffuse!
            result = result * trig->GetMaterial().diffuse;
        }
    }
    return result;
}

float Fresnel(glm::vec3 I, glm::vec3 N, float ior){
    float cosi = glm::dot(I, N);
    float etai = 1.0f, etat = ior;
    if (cosi > 0) { std::swap(etai, etat); }
    // Snell's law
    float sint = etai / etat * glm::sqrt(glm::max(0.f, 1.0f - cosi * cosi));
    if(sint >= 1){
        // Total internal reflection
        return 1.0f;
    }else{
        float cost = sqrtf(std::max(0.f, 1 - sint * sint));
        cosi = fabsf(cosi);
        float Rs = ((etat * cosi) - (etai * cost)) / ((etat * cosi) + (etai * cost));
        float Rp = ((etai * cosi) - (etat * cost)) / ((etai * cosi) + (etat * cost));
        return (Rs * Rs + Rp * Rp) / 2.0f;
    }
}

glm::vec3 Refract(glm::vec3 in, glm::vec3 N, float IOR, bool debug = false){
    (void)debug;
    if(glm::dot(in, N) > 0.999f) return -in;
    glm::vec3 tangent = glm::normalize(glm::cross(N,in));
    float cosEta1 = glm::dot(in,N);
    float sinEta1 = glm::sqrt(1.0f - cosEta1 * cosEta1);
    IFDEBUG std::cout << "Eta1 " << glm::degrees(glm::angle(N, in)) << std::endl;
    IFDEBUG std::cout << "sinEta1 " << sinEta1 << std::endl;
    float sinEta2 = sinEta1 * IOR;
    IFDEBUG std::cout << "sinEta2 " << sinEta2 << std::endl;
    if(sinEta2 >= 1.0f) return glm::vec3(std::numeric_limits<float>::quiet_NaN()); // total internal
    float Eta2 = glm::asin(sinEta2);
    IFDEBUG std::cout << "Eta2 " << glm::degrees(Eta2) << std::endl;
    return glm::rotate(-N, Eta2, tangent);
}

std::vector<PathTracer::PathPoint> PathTracer::GeneratePath(Ray r, unsigned int& raycount, unsigned int depth__, float russian__, bool debug) const {

    std::vector<PathPoint> path;

    IFDEBUG std::cout << "Ray origin: " << r.origin << std::endl;
    IFDEBUG std::cout << "Ray direction: " << r.direction << std::endl;

    Radiance cumulative_transfer_coeff = Radiance(1.0f, 1.0f, 1.0f);

    Ray current_ray = r;
    unsigned int n = 0, n2 = 0;
    // Temporarily setting this to true ensures that russian roulette will not terminate (once).
    bool skip_russian = false;
    // Used for tracking index of refraction
    // float current_ior = 1.0f;
    const Triangle* last_triangle = nullptr;
    while(true){
        n++; n2++;
        if(n2 >= 20) break; // hard limit
        if(russian__ >= 0.0f){
            // Russian roulette path termination
            if(n > 1 && !skip_russian && sampler.Get1D() > russian__) break;
            skip_russian = false;
        }else{
            // Fixed depth path termination
            if(n > depth__) break;
        }

        IFDEBUG std::cout << "Generating path, n = " << n << std::endl;

        raycount++;
        Intersection i;
        if(scene.thinglass.size() == 0){
            // This variant is a bit faster.
            i = scene.FindIntersectKdOtherThan(current_ray, last_triangle);
        }else{
            i = scene.FindIntersectKdOtherThanWithThinglass(current_ray, last_triangle);
        }
        PathPoint p;
        p.thinglass_isect = i.thinglass;
        if(!i.triangle){
            // A sky ray!
            IFDEBUG std::cout << "Sky ray!" << std::endl;
            p.infinity = true;
            p.Vr = -current_ray.direction;
            qassert_false(std::isnan(p.Vr.x));
            path.push_back(p);
            // End path.
            break;
        }else{
            if(i.triangle == last_triangle){
                // std::cerr << "Ray collided with source triangle. This should never happen." << std::endl;
            }
            // Prepare normal
            assert(NEAR(glm::length(current_ray.direction),1.0f));
            p.pos = current_ray[i.t];
            p.faceN = i.Interpolate(i.triangle->GetNormalA(),
                                    i.triangle->GetNormalB(),
                                    i.triangle->GetNormalC());

            if(std::isnan(p.faceN.x)){
                // Ah crap. Assimp incorrectly merged some vertices.
                // Just try another normal.
                p.faceN = i.triangle->GetNormalA();
                if(std::isnan(p.faceN.x)){
                    p.faceN = i.triangle->GetNormalB();
                    if(std::isnan(p.faceN.x)){
                        p.faceN = i.triangle->GetNormalC();
                        if(std::isnan(p.faceN.x)){
                            // All three vertices are messed up? Not much we can help now. Let's just ignore this ray.
                            return path;
                        }
                    }
                }
            }
            // Sometimes it may happen, when interpolating between reverse vectors,
            // the the the result is 0. Or worse: some models contain zero-length normal vectors!
            // In such unfortunate case, just igore this ray.
            if(glm::length(p.faceN) <= 0.0f){
                return path;
            }

            p.faceN = glm::normalize(p.faceN);
            // Prepare incoming direction
            p.Vr = -current_ray.direction;
            qassert_false(std::isnan(p.Vr.x));

            const Material& mat = i.triangle->GetMaterial();
            p.mat = &mat;

            bool fromInside = false;
            if(glm::dot(p.faceN, p.Vr) < 0){
                fromInside = true;
                // Pretend correction.
                p.faceN = -p.faceN;
                p.backside = true;
            }

            assert(!std::isnan(p.faceN.x));

            glm::vec2 texUV;
            // Interpolate textures
            if(mat.ambient_texture || mat.diffuse_texture || mat.specular_texture || mat.bump_texture){
                glm::vec2 a = i.triangle->GetTexCoordsA();
                glm::vec2 b = i.triangle->GetTexCoordsB();
                glm::vec2 c = i.triangle->GetTexCoordsC();
                texUV = i.Interpolate(a,b,c);
                IFDEBUG std::cout << "texUV = " << texUV << std::endl;
            }
            // Get colors from texture
            p.diffuse  =  mat.diffuse_texture?mat.diffuse_texture->GetPixelInterpolated(texUV,debug) : mat.diffuse ;
            p.specular = mat.specular_texture?mat.specular_texture->GetPixelInterpolated(texUV,debug): mat.specular;
            // Tilt normal using bump texture
            if(mat.bump_texture){
                float right = mat.bump_texture->GetSlopeRight(texUV);
                float bottom = mat.bump_texture->GetSlopeBottom(texUV);
                glm::vec3 tangent = i.Interpolate(i.triangle->GetTangentA(),
                                                  i.triangle->GetTangentB(),
                                                  i.triangle->GetTangentC());
                if(tangent.x*tangent.x + tangent.y*tangent.y + tangent.z*tangent.z < 0.001f){
                    // Well, so apparently, sometimes assimp generates invalid tangents. They seem okay
                    // on their own, but they interpolate weird, because tangents at two coincident vertices
                    // are opposite. Thus if it happens that interpolated tangent is zero, and therefore can't be
                    // normalized, we just silently ignore the bump map in this point. I'll have little effect on the
                    // entire pixel anyway.
                    p.lightN = p.faceN;
                }else{
                    tangent = glm::normalize(tangent);
                    glm::vec3 bitangent = glm::normalize(glm::cross(p.faceN,tangent));
                    glm::vec3 tangent2 = glm::cross(bitangent,p.faceN);
                    p.lightN = glm::normalize(p.faceN + (tangent2*right + bitangent*bottom) * bumpmap_scale);
                    IFDEBUG std::cout << "faceN " << p.faceN << std::endl;
                    IFDEBUG std::cout << "lightN " << p.lightN << std::endl;
                    // This still happend.
                    if(glm::isnan(p.lightN.x)){
                        p.lightN = p.faceN;
                    }
                    assert(glm::length(p.lightN) > 0);
                    float dot2 = glm::dot(p.faceN,tangent2);
                    assert(dot2 >= -0.001f);
                    float dot3 = glm::dot(p.faceN,bitangent);
                    assert(dot3 >= -0.001f);
                    float dot = glm::dot(p.faceN,p.lightN);
                    assert(dot > 0);
                }
            }else{
                p.lightN = p.faceN;
            }

            assert(!std::isnan(p.lightN.x));

            // Randomly determine point type
            float ptype_sample = sampler.Get1D();
            if(mat.translucency > 0.001f){
                // This is a translucent material.
                if(fromInside){
                    // Ray leaves the object
                    p.type = PathPoint::LEFT;
                }else{
                    float q = Fresnel(p.Vr, p.lightN, 1.0/mat.refraction_index);
                    if(RandomUtils::DecideAndRescale(ptype_sample, q))
                        p.type = PathPoint::REFLECTED;
                    else{
                        if(RandomUtils::DecideAndRescale(ptype_sample, mat.translucency))
                            p.type = PathPoint::ENTERED;
                        else p.type = PathPoint::SCATTERED;
                    }
                }
            }else{
                // Not a translucent material.
                if(force_fresnell){
                    float strength =
                        (p.specular.r + p.specular.g + p.specular.b)/
                        (p.diffuse .r + p.diffuse .g + p.diffuse .b +
                         p.specular.r + p.specular.g + p.specular.b);
                    if(RandomUtils::DecideAndRescale(ptype_sample, strength) &&
                       RandomUtils::DecideAndRescale(ptype_sample, Fresnel(p.Vr, p.lightN, 1.0/mat.refraction_index)))
                        p.type = PathPoint::REFLECTED;
                    else
                        p.type = PathPoint::SCATTERED;
                }else{
                    p.type = PathPoint::SCATTERED;
                }
            }

            // Skip roulette if the ray has priviledge
            if(p.type == PathPoint::REFLECTED ||
               p.type == PathPoint::ENTERED ||
               p.type == PathPoint::LEFT){
                // Do not count this point into depth. Never russian-terminate path at this point.
                IFDEBUG std::cout << "Not counting this point" << std::endl;
                n--; skip_russian = true;
            }

            BRDF::BRDFSamplingType sampling_type = BRDF::SAMPLING_COSINE;
            p.transfer_coefficients = Radiance(1.0f, 1.0f, 1.0f);
            // Compute next ray direction
            IFDEBUG std::cout << "Ray hit material " << mat.name << " at " << p.pos << " and ";
            glm::vec3 dir;
            int counter = 0;
            (void)counter;
            glm::vec2 sample;
            switch(p.type){
            case PathPoint::REFLECTED:
                IFDEBUG std::cout << "REFLECTED." << std::endl;
                dir = 2.0f * glm::dot(p.Vr, p.lightN) * p.lightN - p.Vr;
                if(glm::dot(dir, p.faceN) > 0.0f)
                    break;
                // Otherwise, this reflected ray would enter inside the face.
                // Therefore, pretend it's a scatter ray. Thus:
                /* FALLTHROUGH */
            case PathPoint::SCATTERED:
                IFDEBUG std::cout << "SCATTERED." << std::endl;
                // Revert to face normal in case this ray would enter from inside
                if(glm::dot(p.lightN, p.Vr) <= 0.0f) p.lightN = p.faceN;

                sample = sampler.Get2D();
                std::tie(dir, p.transfer_coefficients, sampling_type) = mat.brdf->GetRay(p.lightN, p.Vr, Radiance(p.diffuse), Radiance(p.specular), sample, debug);
                if(!(glm::dot(dir, p.faceN) > 0.0f)){
                    std::cout << "dir: " << dir << std::endl;
                    std::cout << "lightN: " << p.lightN << std::endl;
                    std::cout << "faceN: " << p.faceN << std::endl;
                    std::cout << "mat name: " << mat.name << std::endl;

                    std::tie(dir, p.transfer_coefficients, sampling_type) = mat.brdf->GetRay(p.lightN, p.Vr, Radiance(p.diffuse), Radiance(p.specular), sample, true);
                }
                qassert_true(glm::dot(dir, p.faceN) > 0.0f);
                /* THE FOLLOWING wastes a large number of random samples!
                do{
                    std::tie(dir, p.transfer_coefficients, sampling_type) = mat.brdf->GetRay(p.lightN, p.Vr, Radiance(p.diffuse), Radiance(p.specular), rnd, debug);
                    counter++;
                }while(glm::dot(dir, p.faceN) <= 0.0f && counter < 20);
                if(counter == 20){
                    // We have tried 20 different samples, and they all would enter the face.
                    // This happens if brdf is very narrowly distributed.
                    // In this case, we technically need a better sampling strategy.
                    // Possible options are: 1) terminate path right here 2) revert to the face normal vector 3) other.
                    // Unfortunatelly, I do not have the time right now to consider and compare these options.
                    // So, temporarily, I'll do 2).
                    do{ //                                          Notice faceN here ---\/---
                        std::tie(dir, p.transfer_coefficients, sampling_type) = mat.brdf->GetRay(p.faceN, p.Vr, Radiance(p.diffuse), Radiance(p.specular), rnd);
                    }while(glm::dot(dir, p.faceN) <= 0.0f);
                }
                */
                break;
            case PathPoint::ENTERED:
                IFDEBUG std::cout << "ENTERED medium." << std::endl;
                dir = Refract(p.Vr, p.lightN, 1.0f/mat.refraction_index, debug);
                if(glm::isnan(dir.x)){
                    // Internal reflection
                    IFDEBUG std::cout << "internally reflected." << std::endl;
                    p.type = PathPoint::REFLECTED;
                    dir = 2.0f * glm::dot(p.Vr, p.lightN) * p.lightN - p.Vr;
                }
                break;
            case PathPoint::LEFT:
                // TODO: Refraction
                IFDEBUG std::cout << "LEFT medium." << std::endl;
                dir = Refract(p.Vr, p.lightN, mat.refraction_index, debug);
                if(glm::isnan(dir.x)){
                    // Internal reflection
                    IFDEBUG std::cout << "internally reflected." << std::endl;
                    p.type = PathPoint::REFLECTED;
                    dir = 2.0f * glm::dot(p.Vr, p.lightN) * p.lightN - p.Vr;
                }
                break;
            }
            p.Vi = dir;

            // Store russian coefficient
            if(russian__ > 0.0f && !skip_russian) p.russian_coefficient = 1.0f/russian__;
            else p.russian_coefficient = 1.0f;

            // Calculate transfer coefficients (BRFD, cosine, etc.)
            if(p.type == PathPoint::SCATTERED){
                // Branch prediction should optimize-out these conditional jump during runtime.
                if(sampling_type != BRDF::SAMPLING_COSINE){
                    // All sampling types use cosine, but for cosine sampling probability
                    // density is equal to cosine, so they cancel out.
                    IFDEBUG std::cout << "Mult by cos" << std::endl;
                    float cos = glm::dot(p.lightN, p.Vi);
                    // TODO: Investigate this condition.
                    // Equations (confirmed by total lighting compedium) clearly state that brdf sampling
                    // still requires multiplying by cosine. However, enabling it results in an ugly dark border
                    // around a reflective sphere. Should this condition be here? Maybe it should be applied
                    // differently in various cases?
                    if(sampling_type != BRDF::SAMPLING_BRDF)
                        p.transfer_coefficients *= cos;
                }else{
                    // Cosine sampling p = cos/pi. Don't divide by cos, as it was
                    // skipped, instead just multiply by pi.
                    p.transfer_coefficients *= glm::pi<float>();
                }
                if(sampling_type != BRDF::SAMPLING_BRDF){
                    // All sampling types use brdf, but for brdf sampling probability
                    // density is equal to brdf, so they cancel out.
                    IFDEBUG std::cout << "Mult by f" << std::endl;
                    Radiance f = mat.brdf->Apply(p.diffuse, p.specular, p.lightN, p.Vi, p.Vr, debug);
                    p.transfer_coefficients *= f;
                }
                if(sampling_type != BRDF::SAMPLING_UNIFORM){
                    // NOP
                }else{
                    // Probability density for uniform sampling.
                    IFDEBUG std::cout << "Div by P" << std::endl;
                    p.transfer_coefficients *= glm::pi<float>()/0.5;
                }
            }

            // TODO: Terminate when cumulative_coeff gets too low
            cumulative_transfer_coeff *= p.russian_coefficient;
            cumulative_transfer_coeff *= p.transfer_coefficients;
            IFDEBUG std::cout << "Path cumulative transfer coeff: " << cumulative_transfer_coeff << std::endl;

            // Commit the path point to the path
            path.push_back(p);

            // Prepate next ray
            current_ray = Ray(p.pos +
                              p.faceN * scene.epsilon * 10.0f *
                              ((p.type == PathPoint::ENTERED || p.type == PathPoint::LEFT)?-1.0f:1.0f)
                              , glm::normalize(dir));
            qassert_false(std::isnan(current_ray.direction.x));

            IFDEBUG std::cout << "Next ray will be from " << p. pos << " dir " << dir << std::endl;

            last_triangle = i.triangle;
            // Continue for next ray
        }
    }

    return path;
}

PixelRenderResult PathTracer::TracePath(const Ray& r, unsigned int& raycount, bool debug){
    PixelRenderResult result;

    glm::vec3 camerapos = r.origin;

    // ===== 1st Phase =======
    // Generate a forward path.
    IFDEBUG std::cout << "== FORWARD PATH" << std::endl;
    std::vector<PathPoint> path = GeneratePath(r, raycount, depth, russian, debug);

    std::vector<Light> lights;

    // Choose a main light source.
    glm::vec2 areal_sample = sampler.Get2D();
    glm::vec2 lightdir_sample = sampler.Get2D();
    lights.push_back(scene.GetRandomLight(sampler.Get2D(), sampler.Get1D(), areal_sample));

    IFDEBUG std::cout << "-------- Areal sample:" << areal_sample << std::endl;

    // Choose auxiculary light sources
    /*
    const int AUX_LIGHTS = 4;
    for(unsigned int i = 0; i < AUX_LIGHTS; i++)
        lights.push_back(scene.GetRandomLight(rnd));
    */

    // Generate backward path (from light)
    Light& main_light = lights[0];
    glm::vec3 main_light_dir;
    if(main_light.type == Light::FULL_SPHERE){
        glm::vec3 dir = RandomUtils::Sample2DToSphereUniform(areal_sample);
        // TODO: Can this be done without modifying light position?
        main_light.pos += main_light.size * dir;
        main_light_dir = RandomUtils::Sample2DToHemisphereCosineDirected(lightdir_sample, glm::normalize(dir));
    }else{
        main_light_dir = RandomUtils::Sample2DToHemisphereCosineDirected(lightdir_sample, main_light.normal);
    }
    IFDEBUG std::cout << "== LIGHT PATH" << std::endl;
    Ray light_ray(main_light.pos + scene.epsilon * main_light.normal * 100.0f, main_light_dir);
    std::vector<PathPoint> light_path = GeneratePath(light_ray, raycount, reverse, -1.0f, debug);
    IFDEBUG std::cout << "Light path size " << light_path.size() << std::endl;

    // ============== 2nd phase ==============
    // Calculate light transmitted over light path.
    Radiance light_carried;

    IFDEBUG std::cout << " === Carrying light along light path" << std::endl;

    for(unsigned int n = 0; n < light_path.size(); n++){
        PathPoint& p = light_path[n];

        if(n == 0){
            //glm::vec3 Vi = glm::normalize(lightpos - p.pos);
            //float G = glm::max(0.0f, glm::dot(p.lightN, Vi)) / glm::distance2(lightpos, p.pos);
            //IFDEBUG std::cout << "G = " << G << std::endl;
            IFDEBUG std::cout << "main_light.pos = " << main_light.pos << std::endl;
            IFDEBUG std::cout << "p.pos = " << p.pos << std::endl;

            light_carried =
                Radiance(main_light.color) *
                main_light.intensity *
                main_light.GetDirectionalFactor(main_light_dir); // * G;
        }

        light_carried = ApplyThinglass(light_carried, p.thinglass_isect, p.Vr);

        p.light_from_source = light_carried;

        if(p.type == PathPoint::SCATTERED){
            light_carried *= p.transfer_coefficients * p.russian_coefficient;
        }else if(p.type == PathPoint::REFLECTED || p.type == PathPoint::LEFT){
            // NOP
        }else if(p.type == PathPoint::ENTERED){
            IFDEBUG std::cout << "Multiplying carried light by " << p.diffuse << std::endl;
            light_carried = light_carried * p.diffuse;
        }

        IFDEBUG std::cout << "After light point " << n << ", carried light:" << light_carried << std::endl;

        if(p.type == PathPoint::SCATTERED){
            // Connect the point with camera and add as a side effect
            if(!p.infinity && scene.Visibility(p.pos, camerapos)){
                IFDEBUG std::cout << "Point " << p.pos << " is visible from camera." << std::endl;
                glm::vec3 direction = glm::normalize(p.pos - camerapos);
                Radiance q = light_carried * p.mat->brdf->Apply(p.diffuse, p.specular, p.lightN, p.Vr, -direction, debug);
                float G = glm::max(0.0f, glm::dot(p.lightN, -direction)) / glm::distance2(camerapos, p.pos);
                IFDEBUG std::cout << "G = " << G << std::endl;
                if(G >= 0.00001f && !std::isnan(q.r)){
                    q *= G;
                    int x2, y2;
                    IFDEBUG std::cout << "Side effect from " << direction << std::endl;
                    bool in_view = camera.GetCoordsFromDirection( direction, x2, y2, debug);
                    if(in_view){
                        IFDEBUG std::cout << "In view at " << x2 << " " << y2 << ", radiance: " << q << std::endl;
                        result.side_effects.push_back(std::make_tuple(x2, y2, q));
                    }
                }
            }
        }
    }

    //return result;

    // ============== 3rd phase ==============
    // Calculate light transmitted over view path.

    Radiance from_next;

    for(int n = path.size()-1; n >= 0; n--){
        IFDEBUG std::cout << "--- Processing PP " << n << std::endl;

        bool last = ((unsigned int)n == path.size()-1);
        const PathPoint& p = path[n];
        if(p.infinity){
            qassert_false(std::isnan(p.Vr.x));
            Radiance sky_radiance = scene.GetSkyboxRay(p.Vr, debug);
            IFDEBUG std::cout << "This a sky ray, total: " << sky_radiance << std::endl;
            from_next = ApplyThinglass(sky_radiance, p.thinglass_isect, -p.Vr);
            continue;
        }

        const Material& mat = *p.mat;

        IFDEBUG std::cout << "Hit material: " << mat.name << std::endl;

        Radiance total(0.0,0.0,0.0);

        if(p.type == PathPoint::SCATTERED){
            // ==========
            // Direct lighting

            for(unsigned int lightno = 0; lightno < lights.size(); lightno++){
                const Light& light = lights[lightno];
                IFDEBUG std::cout << "Incorporating direct lighting component for light "
                                  << lightno << ", light.pos: " << light.pos << std::endl;

                std::vector<std::pair<const Triangle*, float>> thinglass_isect;
                // Visibility factor
                if((scene.thinglass.size() == 0 && scene.Visibility(light.pos, p.pos)) ||
                   (scene.thinglass.size() != 0 && scene.VisibilityWithThinglass(light.pos, p.pos, thinglass_isect))){

                    IFDEBUG std::cout << "====> Light is visible" << std::endl;

                    // Incoming direction
                    glm::vec3 Vi = glm::normalize(light.pos - p.pos);

                    Radiance f = mat.brdf->Apply(p.diffuse, p.specular, p.lightN, Vi, p.Vr, debug);

                    IFDEBUG std::cout << "f = " << f << std::endl;

                    float G = glm::max(0.0f, glm::dot(p.lightN, Vi)) / glm::distance2(light.pos, p.pos);
                    IFDEBUG std::cout << "G = " << G << ", angle " << glm::angle(p.lightN, Vi) << std::endl;
                    Radiance inc_l = Radiance(light.color) * light.intensity * light.GetDirectionalFactor(-Vi);
                    inc_l = ApplyThinglass(inc_l, thinglass_isect, Vi);

                    IFDEBUG std::cout << "incoming light with filters: " << inc_l << std::endl;

                    Radiance out = inc_l * f * G;
                    IFDEBUG std::cout << "total direct lighting: " << out << std::endl;
                    total += out;
                }else{
                    IFDEBUG std::cout << "Light not visible" << std::endl;
                }
            }

            // Reverse light
            for(unsigned int q = 0; q < light_path.size(); q++){
                const PathPoint& l = light_path[q];
                // TODO: Thinglass?
                if(!l.infinity && scene.Visibility(l.pos, p.pos)){
                    glm::vec3 light_to_p = glm::normalize(p.pos - l.pos);
                    glm::vec3 p_to_light = -light_to_p;
                    Radiance f_light = l.mat->brdf->Apply(l.diffuse, l.specular, l.lightN, light_to_p, l.Vr, debug);
                    Radiance f_point = p.mat->brdf->Apply(p.diffuse, p.specular, p.lightN, p.Vr, p_to_light, debug);
                    float G = glm::max(0.0f, glm::dot(p.lightN, p_to_light)) / glm::distance2(l.pos, p.pos);
                    total += l.light_from_source * f_light * f_point * G;
                }// not visible from each other.
            }

            IFDEBUG std::cerr << "total with light path: " << total << std::endl;

            // =================
            // Indirect lighting
            if(!last){
                // look at next pp's to_prev and incorporate it here
                Radiance inc = from_next;
                IFDEBUG std::cout << "Incorporating indirect lighting - incoming radiance: " << inc << std::endl;
                inc = inc * p.russian_coefficient * p.transfer_coefficients;
                IFDEBUG std::cout << "Incoming * brdf * cos(...) / sampleP = " << inc << std::endl;
                total += inc;
            }
        }else if(p.type == PathPoint::REFLECTED || p.type == PathPoint::LEFT){
            assert(path.size() >= (unsigned int)n+1);
            total += from_next;
        }else if(p.type == PathPoint::ENTERED){
            assert(path.size() >= (unsigned int)n+1);
            // Note: Cannot use Kt factor from mtl file as assimp does not support it.
            total += from_next * p.diffuse;
        }

        IFDEBUG std::cerr << "total after direct and indirect: " << total << std::endl;

        if(mat.emissive && !p.backside){
            total += Radiance(mat.emission);
        }

        //IFDEBUG std::cerr << "total with emission: " << total << std::endl;

        // Finally, apply thinglass filters that were encountered
        // when we were looking for intersection and stumbled upon this particular PP.

        total = ApplyThinglass(total, p.thinglass_isect, p.Vr);

        //IFDEBUG std::cerr << "total with thinglass filters: " << total << std::endl;

        // Clamp.
        if(total.r > clamp) total.r = clamp;
        if(total.g > clamp) total.g = clamp;
        if(total.b > clamp) total.b = clamp;

        // Safeguard against any accidental nans or negative values.
        if(glm::isnan(total.r) || total.r < 0.0f) total.r = 0.0f;
        if(glm::isnan(total.g) || total.g < 0.0f) total.g = 0.0f;
        if(glm::isnan(total.b) || total.b < 0.0f) total.b = 0.0f;

        IFDEBUG std::cerr << "total clamped: " << total << std::endl;

        from_next = total;

    } // for each point on path
    IFDEBUG std::cerr << "PATH TOTAL" << from_next << std::endl << std::endl;
    result.main_pixel = from_next;
    return result;
}
