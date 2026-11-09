#include "render_image.h"
#include "document.h"
#include "types.h"

litehtml::pixel_t litehtml::render_item_image::_render(pixel_t x, pixel_t y,
                                                       const containing_block_context& containing_block_size,
                                                       formatting_context* /*fmt_ctx*/, bool /*second_pass*/)
{
    pixel_t                  parent_width = containing_block_size.width;
    containing_block_context self_size    = calculate_containing_block_context(containing_block_size);

    calc_outlines(parent_width);

    m_pos.move_to(x, y);

    document::ptr doc = src_el()->get_document();

    litehtml::size sz;
    src_el()->get_content_size(sz, containing_block_size.width);

    m_pos.width  = sz.width;
    m_pos.height = sz.height;

    src_el()->css().line_height().computed_value = height();

    if(src_el()->css().get_height().is_predefined() && src_el()->css().get_width().is_predefined())
    {
        m_pos.height = sz.height;
        m_pos.width  = sz.width;

        // check for max-width
        if(!src_el()->css().get_max_width().is_predefined())
        {
            pixel_t max_width = doc->to_pixels(css().get_max_width(), css().get_font_metrics(), parent_width);
            if(m_pos.width > max_width)
            {
                m_pos.width = max_width;
            }
            if(sz.width != 0_px)
            {
                m_pos.height = m_pos.width * sz.height / sz.width;
            } else
            {
                m_pos.height = sz.height;
            }
        }

        // check for max-height
        if(!src_el()->css().get_max_height().is_predefined())
        {
            pixel_t max_height = calc_max_height(sz.height, containing_block_size.height);
            if(m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
            if(sz.height != 0_px)
            {
                m_pos.width = m_pos.height * sz.width / sz.height;
            } else
            {
                m_pos.width = sz.width;
            }
        }
    } else if(!src_el()->css().get_height().is_predefined() && src_el()->css().get_width().is_predefined())
    {
        if(self_size.height.type != containing_block_context::cbc_value_type_auto && self_size.height.value > 0_px)
        {
            m_pos.height = self_size.height;
        }

        // check for max-height
        if(!src_el()->css().get_max_height().is_predefined())
        {
            pixel_t max_height = calc_max_height(sz.height, containing_block_size.height);
            if(m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
        }

        if(sz.height != 0_px)
        {
            m_pos.width = m_pos.height * sz.width / sz.height;
        } else
        {
            m_pos.width = sz.width;
        }
    } else if(src_el()->css().get_height().is_predefined() && !src_el()->css().get_width().is_predefined())
    {
        m_pos.width = src_el()->css().get_width().calc_percent(parent_width);

        // check for max-width
        if(!src_el()->css().get_max_width().is_predefined())
        {
            pixel_t max_width = doc->to_pixels(css().get_max_width(), css().get_font_metrics(), parent_width);
            if(m_pos.width > max_width)
            {
                m_pos.width = max_width;
            }
        }

        if(sz.width != 0_px)
        {
            m_pos.height = m_pos.width * sz.height / sz.width;
        } else
        {
            m_pos.height = sz.height;
        }
    } else
    {
        m_pos.width  = src_el()->css().get_width().calc_percent(parent_width);
        m_pos.height = 0_px;
        if(self_size.height.type != containing_block_context::cbc_value_type_auto && self_size.height.value > 0_px)
        {
            m_pos.height = self_size.height;
        }

        // check for max-height
        if(!src_el()->css().get_max_height().is_predefined())
        {
            pixel_t max_height = calc_max_height(sz.height, containing_block_size.height);
            if(m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
        }

        // check for max-height
        if(!src_el()->css().get_max_width().is_predefined())
        {
            pixel_t max_width = doc->to_pixels(css().get_max_width(), css().get_font_metrics(), parent_width);
            if(m_pos.width > max_width)
            {
                m_pos.width = max_width;
            }
        }
    }

    m_pos.x += content_offset_left();
    m_pos.y += content_offset_top();

    return m_pos.width + content_offset_width();
}

litehtml::pixel_t litehtml::render_item_image::calc_max_height(pixel_t image_height, pixel_t containing_block_height)
{
    document::ptr doc = src_el()->get_document();
    return doc->to_pixels(css().get_max_height(), css().get_font_metrics(),
                          containing_block_height == 0_px ? image_height : containing_block_height);
}

void litehtml::render_item_image::calc_intrinsic_size()
{
    litehtml::size sz;

    src_el()->get_content_size(sz, 0_px);

    // The size it's drawn at, as _render() works it out, from what doesn't depend on the containing
    // block: a width or height in fixed units, and max-width and max-height, keeping the image's
    // proportions. Its natural size alone made a table column as wide as the natural width of an
    // image shown smaller (a wide logo with max-height: 17mm), so a cell sized for it pushed the
    // image off the page.
    document::ptr doc   = src_el()->get_document();
    const auto&   fm    = css().get_font_metrics();
    auto          fixed = [](const css_length& length) {
        return !length.is_predefined() && length.units() != css_units_percentage;
    };
    pixel_t    width     = sz.width;
    pixel_t    height    = sz.height;
    const bool has_width = fixed(css().get_width());
    const bool has_height = fixed(css().get_height());
    if(has_width)
    {
        width = doc->to_pixels(css().get_width(), fm, 0_px);
    }
    if(has_height)
    {
        height = doc->to_pixels(css().get_height(), fm, 0_px);
    }
    if(has_width && !has_height)
    {
        height = sz.width != 0_px ? width * sz.height / sz.width : sz.height;
    } else if(has_height && !has_width)
    {
        width = sz.height != 0_px ? height * sz.width / sz.height : sz.width;
    }
    if(!has_width && fixed(css().get_max_width()))
    {
        const pixel_t max_width = doc->to_pixels(css().get_max_width(), fm, 0_px);
        if(width > max_width)
        {
            width = max_width;
            if(!has_height && sz.width != 0_px)
            {
                height = width * sz.height / sz.width;
            }
        }
    }
    if(!has_height && fixed(css().get_max_height()))
    {
        const pixel_t max_height = doc->to_pixels(css().get_max_height(), fm, 0_px);
        if(height > max_height)
        {
            height = max_height;
            if(!has_width && sz.height != 0_px)
            {
                width = height * sz.width / sz.height;
            }
        }
    }

    m_intrinsic_min_size.width = m_intrinsic_max_size.width = width;
    m_intrinsic_min_size.height = m_intrinsic_max_size.height = height;

    // A percentage width or max-width lets it shrink with its container, as browsers do (a
    // "compressible" replaced element, CSS Sizing 3, 5.2.2): it asks for no width of its own.
    if(!has_width && !css().get_max_width().is_predefined() && css().get_max_width().units() == css_units_percentage)
    {
        m_intrinsic_min_size.width = 0_px;
    }
}
