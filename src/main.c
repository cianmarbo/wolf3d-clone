
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>

#include <SDL2/SDL.h>

#define WINDOW_WIDTH            800
#define WINDOW_HEIGHT           600

#define MAP_NUM_ROWS            11
#define MAP_NUM_COLS            15

#define TILE_SIZE               32
#define MINIMAP_SCALE_FACTOR    0.6

#define FPS                     30
#define FRAME_TIME_LENGTH       (1000 / FPS)

#define PI                      3.14159
#define FOV_ANGLE               (60 * (PI / 180))

#define WALL_STRIP_WIDTH        10
#define NUM_RAYS                WINDOW_WIDTH

#define FRAME_BUFFER_SIZE_BYTES (sizeof(uint32_t) * (WINDOW_WIDTH * WINDOW_HEIGHT))

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* texture;

uint32_t* frame_buffer;

bool program_running;

int ticksLastFrame = 0;

/**
 * The below values are from early in development when I was esting player movement on the screen.
 * I never removed them or used them for anything since assuming they would have no effect.
 * 
 * Oh boy, was I wrong. When these two bad boys are initialized to 0 they cause a super weird
 * visual effect when you attempt to move the player forward - a space opens up between the walls
 * right in the centre of the screen. 
 * 
 * They aren't referenced anywhere in the code and so I'm guessing this is some weird memory
 * initialization issue. Also I will note I have only produced this bug on macOS 12.6.7 because
 * who knows, maybe this is an OS specific bug. I guess time will tell :D
 * 
 * This needs further investigation and I just want to document this bug for my own sanity.
 */

// int playerX = 0;
// int playerY = 0;

int minimap_array[11][15] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1},
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1},
    {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}
};

struct Ray {
    float rayAngle;
    float wallHitX;
    float wallHitY;
    float distance;
    bool wasHitVertical;
    bool isRayFacingDown;
    bool isRayFacingUp;
    bool isRayFacingRight;
    bool isRayFacingLeft;
};

struct Ray* ray_array;

struct Player {
    float xPos, yPos;
    int width, height; //player is a rect
    int turnDirection;
    int walkDirection;
    float rotationAngle;
    float moveSpeed;
    float rotationSpeed;
} player;

bool checkCollision(float xPos, float yPos);
void draw_rect(int x_pos, int y_pos, int width, int height, int color);
void draw_unfilled_rect(int x_pos, int y_pos, int width, int height, int color);

void render3DProjectedWalls() {
    for(int i = 0; i < NUM_RAYS; i++) {
        struct Ray ray = ray_array[i];

        float rayDistance = ray.distance * cos(ray.rayAngle - player.rotationAngle);

        float distanceToProjectionPlane = (WINDOW_WIDTH / 2) / tan(FOV_ANGLE);

        float wallStripHeight = (TILE_SIZE / rayDistance) * distanceToProjectionPlane;

        unsigned int wallTopPixel = (WINDOW_HEIGHT / 2) - (wallStripHeight / 2);
        wallTopPixel = wallTopPixel < 0 ? 0 : wallTopPixel;

        // debugging found this value overflowed at any other resolution than 800x600, needs investigation
        unsigned int wallBottomPixel = (WINDOW_HEIGHT / 2) + (wallStripHeight / 2);
        wallBottomPixel = wallBottomPixel > WINDOW_HEIGHT ? WINDOW_HEIGHT : wallBottomPixel;

        // draw walls
        for (int y = wallTopPixel; y < wallBottomPixel; y++) {
            frame_buffer[(WINDOW_WIDTH * y) + i] = ray.wasHitVertical ? 0xFFFFFFFF : 0xFFCCCCCC;
        }

        // draw floor 
        for(int y = wallBottomPixel; y < WINDOW_HEIGHT; y++)  {
            frame_buffer[(WINDOW_WIDTH * y) + i] = 0xffff0000;
        }
    }
}

float distanceBetweenPoints(double x1, double y1, double x2, double y2) {
    return sqrt( ((x2 - x1) * (x2 - x1)) + ((y2 - y1) * (y2 - y1)) );
}

float normalizeAngle(float angle) {
    if(angle > (2* PI)) {
        angle = angle - PI;
    }

    if(angle < 0) {
        angle = (2 * PI) + angle;
    }

    return angle;
}

struct Ray* cast(struct Ray* ray, float rayAngle) {

    ray->rayAngle = normalizeAngle(rayAngle);

    ray->isRayFacingDown = rayAngle > 0 && rayAngle < PI;
    ray->isRayFacingUp = !ray->isRayFacingDown;

    ray->isRayFacingRight = rayAngle < 0.5 * PI || rayAngle > 1.5 * PI;
    ray->isRayFacingLeft = !ray->isRayFacingRight;

    float xintercept, yintercept;
    float xstep, ystep;

    /////////////////////////////////////////////////
    // HORIZONTAL RAY-GRID INTERSECTION CODE
    ////////////////////////////////////////////////

    bool foundHorizontalWallHit = false;
    float horizontalWallHitX = 0;
    float horizontalWallHitY = 0;

    // Find y-cord of closest horizontal grid intersection
    yintercept = ((int)(player.yPos / TILE_SIZE) * TILE_SIZE);

    // Check if angle is facing down, if so add TILE_SIZE (32) to original yintercept, else add nothing (0)
    yintercept += ray->isRayFacingDown ? TILE_SIZE : 0;

    // current player xpos + length of the adjacent side of the triangle 
    xintercept = player.xPos + ((yintercept - player.yPos) / tan(ray->rayAngle));

    // Calculate the increment for xstep and ystep
    ystep = TILE_SIZE;

    // if ray is facing up, subtract 32 for each increment of ystep, if facing down, add 32 for each increment
    ystep *= ray->isRayFacingUp ? -1 : 1;

    xstep = TILE_SIZE / tan(rayAngle);

    // if result of xstep is positive, but angle is left, set xstep to be negative
    xstep *= (ray->isRayFacingLeft && xstep > 0) ? -1 : 1;

    // if result of xstep is negative, but angle is right, set xstep to be positive
    xstep *= (ray->isRayFacingRight && xstep <  0) ? -1 : 1;

    float nextHorizontalTouchX = xintercept;
    float nextHorizontalTouchY = yintercept;

    // Increment xstep and ystep until we find a wall

    while(nextHorizontalTouchX >= 0 && nextHorizontalTouchX <= WINDOW_WIDTH && nextHorizontalTouchY >=0 && nextHorizontalTouchY <= WINDOW_HEIGHT) {
        if(checkCollision(nextHorizontalTouchX, nextHorizontalTouchY - (ray->isRayFacingUp ? 1 : 0))) {

            // WE FOUND A WALL HIT
            foundHorizontalWallHit = true;
            // Save co-ordinates of wall hit
            horizontalWallHitX = nextHorizontalTouchX;
            horizontalWallHitY = nextHorizontalTouchY;

            break;
        } else {
            //if no wall was found, continue to increment intersections
            nextHorizontalTouchX += xstep;
            nextHorizontalTouchY += ystep;
        }
    }

    /////////////////////////////////////////////////
    // VERTICAL RAY-GRID INTERSECTION CODE
    ////////////////////////////////////////////////

    bool foundVerticalWallHit = false;
    float verticalWallHitX = 0;
    float verticalWallHitY = 0;

    // Find x-cord of closest vertical grid intersection
    xintercept = ((int)(player.xPos / TILE_SIZE)) * TILE_SIZE;

    //// Check if angle is facing right, if so add TILE_SIZE (32) to original yintercept, else add nothing (0)
    xintercept += ray->isRayFacingRight ? TILE_SIZE : 0;

    // Find y-cord of closest vertical grid intersection
    yintercept = player.yPos + (xintercept - player.xPos) * tan(ray->rayAngle);

    xstep = TILE_SIZE;

    // set xstep to positive 32 if the ray is facing right, and negative 32 if the ray is facing left
    xstep *= ray->isRayFacingRight ? 1 : -1;

    ystep = TILE_SIZE * tan(ray->rayAngle);

    // if result of ystep is positive, but angle is up, set ystep to be negative
    ystep *= (ray->isRayFacingUp && ystep > 0) ? -1 : 1;

    // if result of ystep is negative, but angle is down set ystep to be positive
    ystep *= (ray->isRayFacingDown && ystep <  0) ? -1 : 1;

    float nextVerticalTouchX = xintercept;
    float nextVerticalTouchY = yintercept;

    // if(this.isRayFacingLeft) {
    //     nextVerticalTouchX--;
    // }

    while(nextVerticalTouchX >= 0 && nextVerticalTouchX <= WINDOW_WIDTH && nextVerticalTouchY >= 0 && nextVerticalTouchY <= WINDOW_HEIGHT) {
        if(checkCollision(nextVerticalTouchX - (ray->isRayFacingLeft ? 1 : 0), nextVerticalTouchY)) {

            foundVerticalWallHit = true;
            verticalWallHitX = nextVerticalTouchX;
            verticalWallHitY = nextVerticalTouchY;

            break;
        } else {
            nextVerticalTouchX += xstep;
            nextVerticalTouchY += ystep;
        }
    }

    // get distances of vertical and horizontal intersections with wall

    float horizontalHitDistance = (foundHorizontalWallHit) ? distanceBetweenPoints(player.xPos, player.yPos, horizontalWallHitX, horizontalWallHitY) : INT_MAX;
    float verticalHitDistance = (foundVerticalWallHit) ? distanceBetweenPoints(player.xPos, player.yPos, verticalWallHitX, verticalWallHitY) : INT_MAX;

    // depending on which distance is smaller, assign it to wallHitX
    // only store smallest of the distances
    ray->wallHitX = (horizontalHitDistance < verticalHitDistance) ? horizontalWallHitX : verticalWallHitX;
    ray->wallHitY = (horizontalHitDistance < verticalHitDistance) ? horizontalWallHitY : verticalWallHitY;
    ray->distance = (horizontalHitDistance < verticalHitDistance) ? horizontalHitDistance : verticalHitDistance;

    // hit was vertical only if vertical distance was less than horizontal distance
    ray->wasHitVertical = (verticalHitDistance < horizontalHitDistance);

    return ray;
}

void castAllRays() {

    float rayAngle = player.rotationAngle - (FOV_ANGLE / 2);
    float rayAngleIncrement = FOV_ANGLE / NUM_RAYS;

    for(int i = 0; i < NUM_RAYS; i++) {

        struct Ray* ray = malloc(sizeof(struct Ray));

        ray = cast(ray, rayAngle);

        ray_array[i] = *ray;

        rayAngle += rayAngleIncrement;
    }
}

/**
 * Clears allocated frame buffer to black
 */
void clear_framebuffer() {
    for(int y = 0; y < WINDOW_HEIGHT; y++) {
        for(int x = 0; x < WINDOW_WIDTH; x++) {
            frame_buffer[(WINDOW_WIDTH * y) + x] = 0xff000000;
        }
    }
}

void draw_rect(int, int, int, int, int);

void draw_minimap(void) {
    for(int y = 0; y < MAP_NUM_ROWS; y++) {
        for(int x = 0; x < MAP_NUM_COLS; x++) {
            int tilePosX = x * TILE_SIZE;
            int tilePosY = y * TILE_SIZE;

            int color;

            if(minimap_array[y][x] == 1) {
                color = 0xffff4598;
            } else {
                color = 0xffffffff;
            }

            draw_rect(
                MINIMAP_SCALE_FACTOR * tilePosX, 
                MINIMAP_SCALE_FACTOR * tilePosY, 
                MINIMAP_SCALE_FACTOR * TILE_SIZE, 
                MINIMAP_SCALE_FACTOR * TILE_SIZE, 
                color);
        }
    }
}

void draw_unfilled_rect(int x_pos, int y_pos, int width, int height, int color) {
    if((x_pos < WINDOW_WIDTH) && (y_pos < WINDOW_HEIGHT)) {
		for(int y = y_pos; y < (y_pos + height); y++) {
			for(int x = x_pos; x < (x_pos + width); x++) {
				if((y == y_pos) || (y == ((y_pos + height) - 1))) {
					frame_buffer[(WINDOW_WIDTH * y) + x] = color;
				} else {
					frame_buffer[(WINDOW_WIDTH * y) + x_pos] = color;
					frame_buffer[(WINDOW_WIDTH * y) + ((x_pos + width) - 1)] = color;
				}
			}
		}
	}
}

void draw_rect(int x_pos, int y_pos, int width, int height, int color) {
    for(int y = y_pos; y < (y_pos + height); y++) {
        for(int x = x_pos; x < (x_pos + width); x++) {
            frame_buffer[(WINDOW_WIDTH * y) + x] = color;
        }
    }
}

void draw_pixel(int x_pos, int y_pos, uint32_t color) {
    frame_buffer[(WINDOW_WIDTH * y_pos) + x_pos] = color;
}

void render_frame_buffer(void) {

    SDL_UpdateTexture(texture, NULL, frame_buffer, (WINDOW_WIDTH * sizeof(uint32_t)));

    SDL_RenderCopy(renderer, texture, NULL, NULL);
}

bool init_window(void) {

    if(SDL_InitSubSystem(SDL_INIT_EVERYTHING) != 0) {
		fprintf(stderr, "There was an error initialising SDL.....\n");
		return false;
    }

    window = SDL_CreateWindow(
        "raycaster",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL
    );

    if(!window) {
        fprintf(stderr, "There was an error creating the window.....\n");
		return false;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);
	
	if(!renderer) {
		fprintf(stderr, "There was an error creating the SDL renderer......");
		return false;
	}

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    return true;
}

void playerSetup(void) {
    player.xPos = WINDOW_WIDTH / 2;
    player.yPos = WINDOW_HEIGHT / 2;
    player.width = 10;
    player.height = 10;
    player.moveSpeed = 10.0f;
    player.rotationAngle = PI / 2;
    player.rotationSpeed = (PI / 180) * 60;
    player.walkDirection = 0;
    player.turnDirection = 0;
}

void raySetup(void) {
    ray_array = (struct Ray*)malloc(sizeof(struct Ray) * NUM_RAYS);
}

void setup(void) {

    frame_buffer = (uint32_t*) malloc(FRAME_BUFFER_SIZE_BYTES);

    if(!frame_buffer) {
		fprintf(stderr, "There was an error allocating memory for the frame buffer....\n");
		program_running = false;
		return;
	}
	
	texture = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING,
		WINDOW_WIDTH,
		WINDOW_HEIGHT
	);

    playerSetup();
    raySetup();
}

void process_input(void) {
    SDL_Event event;
    SDL_PollEvent(&event);

    switch(event.type) {
        case SDL_QUIT:
            program_running = false;
            break;
        case SDL_KEYDOWN: {
            if(event.key.keysym.sym == SDLK_ESCAPE) 
                program_running = false;
            if(event.key.keysym.sym == SDLK_UP)
                player.walkDirection = 1;
            if(event.key.keysym.sym == SDLK_DOWN)
                player.walkDirection = -1;
            if(event.key.keysym.sym == SDLK_RIGHT)
                player.turnDirection = 1;
            if(event.key.keysym.sym == SDLK_LEFT)
                player.turnDirection = -1;
            break;
        }
        case SDL_KEYUP: {
            if(event.key.keysym.sym == SDLK_UP)
                player.walkDirection = 0;
            if(event.key.keysym.sym == SDLK_DOWN)
                player.walkDirection = 0;
            if(event.key.keysym.sym == SDLK_RIGHT)
                player.turnDirection = 0;
            if(event.key.keysym.sym == SDLK_LEFT)
                player.turnDirection = 0;
            break;
        }
    }
}

bool checkCollision(float xPos, float yPos) {
    int xIndex = (int)(xPos / TILE_SIZE);
    int yIndex = (int)(yPos / TILE_SIZE);

    if(minimap_array[yIndex][xIndex] == 1) {
        return false;
    }

    return true;
}

void update_player(float deltaTime) {

    player.rotationAngle += player.turnDirection * player.rotationSpeed * deltaTime;
    float moveStep = player.walkDirection * player.moveSpeed * deltaTime;

    float newPlayerX = player.xPos + cos(player.rotationAngle) * moveStep;
    float newPlayerY = player.yPos + sin(player.rotationAngle) * moveStep;

    if(checkCollision(newPlayerX, newPlayerY)) {
        player.xPos = newPlayerX;
        player.yPos = newPlayerY;
    }
}

void update(void) {

    while(!SDL_TICKS_PASSED(SDL_GetTicks(), ticksLastFrame + FRAME_TIME_LENGTH));

    float deltaTime = (SDL_GetTicks() - ticksLastFrame) / 1000.0f; // division by 1000 to convert miliseconds to seconds
    ticksLastFrame = SDL_GetTicks(); // on the next frame, this will be subtracted from that frames ticks, which will be more than this frame

    update_player(deltaTime);
    castAllRays();
}

void render(void) {

    SDL_RenderClear(renderer);
    clear_framebuffer();

    draw_minimap();

    draw_rect(
        MINIMAP_SCALE_FACTOR * player.xPos, 
        MINIMAP_SCALE_FACTOR * player.yPos, 
        MINIMAP_SCALE_FACTOR * player.width, 
        MINIMAP_SCALE_FACTOR * player.height, 
        0xff0000ff);

    render3DProjectedWalls();

    render_frame_buffer();
    SDL_RenderPresent(renderer);
}

void destroy_window(void) {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();
}

void quit(void) {
    free(ray_array);
    free(frame_buffer);
    destroy_window();
}

int main(int argc, char** argv) {

    program_running = init_window();

    setup();

    while(program_running) {
        process_input();
        update();
        render();
    }

    quit();

    return 0;
}