# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Synthetic negative controls for the Form visual gates; no native parity claim."""
import copy
import json
from PIL import Image, ImageDraw
from compare_form_visual import compare_case


def main():
    rect = lambda x,y,w,h: {'x':x,'y':y,'width':w,'height':h}
    field = {'id':'value','root':rect(10,10,80,32),'label':None,'control':rect(10,10,80,32),
             'input':rect(10,10,80,32),'help':None,'extra':None,'feedback':None,'textRects':[]}
    case = {'name':'synthetic','scale':1,'viewport':{'width':100,'height':60},
            'forms':[{'id':'form','root':rect(10,10,80,32),'items':[field]}]}
    image = Image.new('RGB',(100,60),'white')
    ImageDraw.Draw(image).rectangle((10,10,89,41),fill=(240,240,240),outline=(217,217,217))
    baseline = compare_case(case,copy.deepcopy(case),image,image.copy())
    assert baseline['geometry_pass'] and baseline['paint_pass'], 'Identical synthetic candidate must pass'
    shifted = copy.deepcopy(case);shifted['forms'][0]['items'][0]['input']['x'] += 2
    assert not compare_case(case,shifted,image,image)['geometry_pass'], 'Two-pixel shift must fail'
    missing = copy.deepcopy(case);missing['forms'][0]['items'][0]['input'] = None
    assert not compare_case(case,missing,image,image)['geometry_pass'], 'Missing input must fail'
    recolored = image.copy();ImageDraw.Draw(recolored).rectangle((20,20,70,30),fill=(244,240,240))
    assert not compare_case(case,case,image,recolored)['paint_pass'], 'Four-byte fill error must fail'
    absent = image.copy();ImageDraw.Draw(absent).rectangle((10,10,89,41),fill='white')
    assert not compare_case(case,case,image,absent)['paint_pass'], 'Missing surface must fail'
    outside = image.copy();ImageDraw.Draw(outside).rectangle((2,48,8,55),fill=(217,217,217))
    assert not compare_case(case,case,image,outside)['paint_pass'], 'Unexpected outside paint must fail'
    omitted = copy.deepcopy(case);omitted['forms'][0]['items'] = []
    try:
        compare_case(case,omitted,image,image)
    except AssertionError:
        pass
    else:
        raise AssertionError('Omitted field must fail topology gate')
    boundary = Image.new('RGB',(100,60),'white')
    ImageDraw.Draw(boundary).rectangle((20,20,70,30),fill=(252,252,252))
    within_budget = boundary.copy()
    ImageDraw.Draw(within_budget).rectangle((20,20,70,30),fill=(251,251,251))
    accepted = compare_case(case,case,boundary,within_budget)
    assert accepted['paint_pass'] and accepted['coverage_error_pixels'] == 0, 'Foreground delta3/4 must honor the one-byte RGB budget'
    beyond_budget = boundary.copy()
    ImageDraw.Draw(beyond_budget).rectangle((20,20,70,30),fill=(250,250,250))
    rejected = compare_case(case,case,boundary,beyond_budget)
    assert not rejected['paint_pass'] and rejected['coverage_error_pixels'] > 0, 'Foreground delta3/5 exceeds the one-byte RGB budget'
    print(json.dumps({'synthetic_baseline':True,'negative_controls_passed':6,'coverage_boundary_controls_passed':2}))


if __name__ == '__main__':
    main()
