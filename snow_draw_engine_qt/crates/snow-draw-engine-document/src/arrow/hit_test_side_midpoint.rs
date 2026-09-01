use super::*;

pub fn get_binding_side_mid_point<T: ?Sized>(
    binding: (&T, Point),
    bindable: &BindableState,
) -> Point {
    let shape = bindable.shape;
    let side = get_shape_side(normalize_fixed_point(binding.1), shape);
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    const OFFSET: f64 = 0.01;
    const OFFSET_DIAGONAL: f64 = OFFSET * 0.707;

    let midpoint = |a: Point, b: Point| -> Point { [(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0] };

    if shape == BindableShape::Diamond {
        let top_x = bindable.width.floor() / 2.0 + 1.0;
        let top_y = 0.0;
        let right_x = bindable.width;
        let right_y = bindable.height.floor() / 2.0 + 1.0;
        let bottom_x = top_x;
        let bottom_y = bindable.height;
        let left_x = 0.0;
        let left_y = right_y;

        let vertical_radius = (top_x - left_x) * 0.01;
        let horizontal_radius = (right_y - top_y) * 0.01;

        let top = [bindable.x + top_x, bindable.y + top_y];
        let right = [bindable.x + right_x, bindable.y + right_y];
        let bottom = [bindable.x + bottom_x, bindable.y + bottom_y];
        let left = [bindable.x + left_x, bindable.y + left_y];

        let right_corner = [
            [right[0] - vertical_radius, right[1] - horizontal_radius],
            right,
            right,
            [right[0] - vertical_radius, right[1] + horizontal_radius],
        ];
        let bottom_corner = [
            [bottom[0] + vertical_radius, bottom[1] - horizontal_radius],
            bottom,
            bottom,
            [bottom[0] - vertical_radius, bottom[1] - horizontal_radius],
        ];
        let left_corner = [
            [left[0] + vertical_radius, left[1] + horizontal_radius],
            left,
            left,
            [left[0] + vertical_radius, left[1] - horizontal_radius],
        ];
        let top_corner = [
            [top[0] - vertical_radius, top[1] + horizontal_radius],
            top,
            top,
            [top[0] + vertical_radius, top[1] + horizontal_radius],
        ];

        let bottom_right = [right_corner[3], bottom_corner[0]];
        let bottom_left = [bottom_corner[3], left_corner[0]];
        let top_left = [left_corner[3], top_corner[0]];
        let top_right = [top_corner[3], right_corner[0]];

        let bezier_point = |curve: [Point; 4], t: f64| -> Point {
            let one_minus_t = 1.0 - t;
            let b0 = one_minus_t * one_minus_t * one_minus_t;
            let b1 = 3.0 * one_minus_t * one_minus_t * t;
            let b2 = 3.0 * one_minus_t * t * t;
            let b3 = t * t * t;
            [
                b0 * curve[0][0] + b1 * curve[1][0] + b2 * curve[2][0] + b3 * curve[3][0],
                b0 * curve[0][1] + b1 * curve[1][1] + b2 * curve[2][1] + b3 * curve[3][1],
            ]
        };

        let diamond_corner_sample = |flat_corner_index: usize| -> Point {
            let steps = 50.0;
            let i = flat_corner_index as f64;
            let t0 = if i - 1.0 < 0.0 {
                0.0
            } else {
                (i - 1.0) / steps
            };
            let t1 = i / steps;
            let t2 = (i + 1.0) / steps;
            let p0 = bezier_point(right_corner, t0);
            let p1 = bezier_point(right_corner, t1);
            let p2 = bezier_point(right_corner, t2);
            [
                p1[0] + ((p2[0] - p0[0]) * 0.5) / 3.0,
                p1[1] + ((p2[1] - p0[1]) * 0.5) / 3.0,
            ]
        };

        let [x, y] = match side {
            Side::Left => {
                let point = diamond_corner_sample(2);
                [point[0] - OFFSET, point[1]]
            }
            Side::Right => {
                let point = diamond_corner_sample(0);
                [point[0] + OFFSET, point[1]]
            }
            Side::Top => {
                let point = diamond_corner_sample(3);
                [point[0], point[1] - OFFSET]
            }
            Side::Bottom => {
                let point = diamond_corner_sample(1);
                [point[0], point[1] + OFFSET]
            }
            Side::TopRight => {
                let point = midpoint(top_right[0], top_right[1]);
                [point[0] + OFFSET_DIAGONAL, point[1] - OFFSET_DIAGONAL]
            }
            Side::BottomRight => {
                let point = midpoint(bottom_right[0], bottom_right[1]);
                [point[0] + OFFSET_DIAGONAL, point[1] + OFFSET_DIAGONAL]
            }
            Side::BottomLeft => {
                let point = midpoint(bottom_left[0], bottom_left[1]);
                [point[0] - OFFSET_DIAGONAL, point[1] + OFFSET_DIAGONAL]
            }
            Side::TopLeft => {
                let point = midpoint(top_left[0], top_left[1]);
                [point[0] - OFFSET_DIAGONAL, point[1] - OFFSET_DIAGONAL]
            }
        };

        return rotate_point([x, y], bindable_center, bindable.angle);
    }

    if shape == BindableShape::Ellipse {
        let ellipse_center_x = bindable.x + bindable.width / 2.0;
        let ellipse_center_y = bindable.y + bindable.height / 2.0;
        let radius_x = bindable.width / 2.0;
        let radius_y = bindable.height / 2.0;

        let [x, y] = match side {
            Side::Top => [ellipse_center_x, ellipse_center_y - radius_y - OFFSET],
            Side::Right => [ellipse_center_x + radius_x + OFFSET, ellipse_center_y],
            Side::Bottom => [ellipse_center_x, ellipse_center_y + radius_y + OFFSET],
            Side::Left => [ellipse_center_x - radius_x - OFFSET, ellipse_center_y],
            Side::TopRight => {
                let angle = -PI / 4.0;
                [
                    ellipse_center_x + radius_x * angle.cos() + OFFSET_DIAGONAL,
                    ellipse_center_y + radius_y * angle.sin() - OFFSET_DIAGONAL,
                ]
            }
            Side::BottomRight => {
                let angle = PI / 4.0;
                [
                    ellipse_center_x + radius_x * angle.cos() + OFFSET_DIAGONAL,
                    ellipse_center_y + radius_y * angle.sin() + OFFSET_DIAGONAL,
                ]
            }
            Side::BottomLeft => {
                let angle = 3.0 * PI / 4.0;
                [
                    ellipse_center_x + radius_x * angle.cos() - OFFSET_DIAGONAL,
                    ellipse_center_y + radius_y * angle.sin() + OFFSET_DIAGONAL,
                ]
            }
            Side::TopLeft => {
                let angle = -3.0 * PI / 4.0;
                [
                    ellipse_center_x + radius_x * angle.cos() - OFFSET_DIAGONAL,
                    ellipse_center_y + radius_y * angle.sin() - OFFSET_DIAGONAL,
                ]
            }
        };

        return rotate_point([x, y], bindable_center, bindable.angle);
    }

    let radius = 0.01;

    let top = [
        [bindable.x + radius, bindable.y],
        [bindable.x + bindable.width - radius, bindable.y],
    ];
    let right = [
        [bindable.x + bindable.width, bindable.y + radius],
        [
            bindable.x + bindable.width,
            bindable.y + bindable.height - radius,
        ],
    ];
    let bottom = [
        [bindable.x + radius, bindable.y + bindable.height],
        [
            bindable.x + bindable.width - radius,
            bindable.y + bindable.height,
        ],
    ];
    let left = [
        [bindable.x, bindable.y + bindable.height - radius],
        [bindable.x, bindable.y + radius],
    ];
    let top_left_corner = [left[1], top[0]];
    let top_right_corner = [top[1], right[0]];
    let bottom_right_corner = [right[1], bottom[1]];
    let bottom_left_corner = [bottom[0], left[0]];

    let [x, y] = match side {
        Side::Top => {
            let point = midpoint(top[0], top[1]);
            [point[0], point[1] - OFFSET]
        }
        Side::Right => {
            let point = midpoint(right[0], right[1]);
            [point[0] + OFFSET, point[1]]
        }
        Side::Bottom => {
            let point = midpoint(bottom[0], bottom[1]);
            [point[0], point[1] + OFFSET]
        }
        Side::Left => {
            let point = midpoint(left[0], left[1]);
            [point[0] - OFFSET, point[1]]
        }
        Side::TopLeft => {
            let point = midpoint(top_left_corner[0], top_left_corner[1]);
            [point[0] - OFFSET_DIAGONAL, point[1] - OFFSET_DIAGONAL]
        }
        Side::TopRight => {
            let point = midpoint(top_right_corner[0], top_right_corner[1]);
            [point[0] + OFFSET_DIAGONAL, point[1] - OFFSET_DIAGONAL]
        }
        Side::BottomRight => {
            let point = midpoint(bottom_right_corner[0], bottom_right_corner[1]);
            [point[0] + OFFSET_DIAGONAL, point[1] + OFFSET_DIAGONAL]
        }
        Side::BottomLeft => {
            let point = midpoint(bottom_left_corner[0], bottom_left_corner[1]);
            [point[0] - OFFSET_DIAGONAL, point[1] + OFFSET_DIAGONAL]
        }
    };

    rotate_point([x, y], bindable_center, bindable.angle)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Side {
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    TopLeft,
}

#[derive(Clone, Copy, Debug)]
struct SectorConfig {
    center_angle: f64,
    sector_width: f64,
    side: Side,
}

const RECTANGLE_SECTORS: [SectorConfig; 8] = [
    SectorConfig {
        center_angle: 0.0,
        sector_width: 75.0,
        side: Side::Right,
    },
    SectorConfig {
        center_angle: 45.0,
        sector_width: 15.0,
        side: Side::BottomRight,
    },
    SectorConfig {
        center_angle: 90.0,
        sector_width: 75.0,
        side: Side::Bottom,
    },
    SectorConfig {
        center_angle: 135.0,
        sector_width: 15.0,
        side: Side::BottomLeft,
    },
    SectorConfig {
        center_angle: 180.0,
        sector_width: 75.0,
        side: Side::Left,
    },
    SectorConfig {
        center_angle: 225.0,
        sector_width: 15.0,
        side: Side::TopLeft,
    },
    SectorConfig {
        center_angle: 270.0,
        sector_width: 75.0,
        side: Side::Top,
    },
    SectorConfig {
        center_angle: 315.0,
        sector_width: 15.0,
        side: Side::TopRight,
    },
];

const DIAMOND_SECTORS: [SectorConfig; 8] = [
    SectorConfig {
        center_angle: 0.0,
        sector_width: 15.0,
        side: Side::Right,
    },
    SectorConfig {
        center_angle: 45.0,
        sector_width: 75.0,
        side: Side::BottomRight,
    },
    SectorConfig {
        center_angle: 90.0,
        sector_width: 15.0,
        side: Side::Bottom,
    },
    SectorConfig {
        center_angle: 135.0,
        sector_width: 75.0,
        side: Side::BottomLeft,
    },
    SectorConfig {
        center_angle: 180.0,
        sector_width: 15.0,
        side: Side::Left,
    },
    SectorConfig {
        center_angle: 225.0,
        sector_width: 75.0,
        side: Side::TopLeft,
    },
    SectorConfig {
        center_angle: 270.0,
        sector_width: 15.0,
        side: Side::Top,
    },
    SectorConfig {
        center_angle: 315.0,
        sector_width: 75.0,
        side: Side::TopRight,
    },
];

fn normalize_degrees(degrees: f64) -> f64 {
    ((degrees % 360.0) + 360.0) % 360.0
}

fn get_shape_side(fixed_point: Point, shape: BindableShape) -> Side {
    let config = match shape {
        BindableShape::Rectangle => &RECTANGLE_SECTORS[..],
        BindableShape::Diamond | BindableShape::Ellipse => &DIAMOND_SECTORS[..],
    };

    let centered_x = fixed_point[0] - 0.5;
    let centered_y = fixed_point[1] - 0.5;
    let radians = centered_y.atan2(centered_x);
    let degrees = normalize_degrees(radians.to_degrees());

    for sector in config {
        let half = sector.sector_width / 2.0;
        let start = normalize_degrees(sector.center_angle - half);
        let end = normalize_degrees(sector.center_angle + half);
        let in_range = if start <= end {
            degrees >= start && degrees <= end
        } else {
            degrees >= start || degrees <= end
        };
        if in_range {
            return sector.side;
        }
    }

    let mut nearest = config[0];
    let mut nearest_distance = f64::INFINITY;
    for sector in config {
        let mut distance_to_sector = (degrees - sector.center_angle).abs();
        if distance_to_sector > 180.0 {
            distance_to_sector = 360.0 - distance_to_sector;
        }
        if distance_to_sector < nearest_distance {
            nearest_distance = distance_to_sector;
            nearest = *sector;
        }
    }
    nearest.side
}
