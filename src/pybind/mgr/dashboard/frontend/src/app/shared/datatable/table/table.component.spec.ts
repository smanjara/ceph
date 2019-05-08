import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FormsModule } from '@angular/forms';
import { RouterTestingModule } from '@angular/router/testing';

import { NgxDatatableModule } from '@swimlane/ngx-datatable';
import * as _ from 'lodash';

import { configureTestBed } from '../../../../testing/unit-test-helper';
import { ComponentsModule } from '../../components/components.module';
import { CdTableFetchDataContext } from '../../models/cd-table-fetch-data-context';
import { TableComponent } from './table.component';

describe('TableComponent', () => {
  let component: TableComponent;
  let fixture: ComponentFixture<TableComponent>;

  const createFakeData = (n) => {
    const data = [];
    for (let i = 0; i < n; i++) {
      data.push({
        a: i,
        b: i * 10,
        c: !!(i % 2)
      });
    }
    return data;
  };

  const clearLocalStorage = () => {
    component.localStorage.clear();
  };

  configureTestBed({
    declarations: [TableComponent],
    imports: [NgxDatatableModule, FormsModule, ComponentsModule, RouterTestingModule]
  });

  beforeEach(() => {
    fixture = TestBed.createComponent(TableComponent);
    component = fixture.componentInstance;

    component.data = createFakeData(10);
    component.columns = [
      { prop: 'a', name: 'Index' },
      { prop: 'b', name: 'Index times ten' },
      { prop: 'c', name: 'Odd?' }
    ];
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should force an identifier', () => {
    component.identifier = 'x';
    component.forceIdentifier = true;
    component.ngOnInit();
    expect(component.identifier).toBe('x');
    expect(component.sorts[0].prop).toBe('a');
    expect(component.sorts).toEqual(component.createSortingDefinition('a'));
  });

  it('should have rows', () => {
    component.useData();
    expect(component.data.length).toBe(10);
    expect(component.rows.length).toBe(component.data.length);
  });

  it('should have an int in setLimit parsing a string', () => {
    expect(component.limit).toBe(10);
    expect(component.limit).toEqual(jasmine.any(Number));

    const e = { target: { value: '1' } };
    component.setLimit(e);
    expect(component.userConfig.limit).toBe(1);
    expect(component.userConfig.limit).toEqual(jasmine.any(Number));
    e.target.value = '-20';
    component.setLimit(e);
    expect(component.userConfig.limit).toBe(1);
  });

  describe('test search', () => {
    const expectSearch = (keyword: string, expectedResult: object[]) => {
      component.search = keyword;
      component.updateFilter();
      expect(component.rows).toEqual(expectedResult);
      component.updateFilter(true);
    };

    it('should find a particular number', () => {
      expectSearch('5', [{ a: 5, b: 50, c: true }]);
      expectSearch('9', [{ a: 9, b: 90, c: true }]);
    });

    it('should find boolean values', () => {
      expectSearch('true', [
        { a: 1, b: 10, c: true },
        { a: 3, b: 30, c: true },
        { a: 5, b: 50, c: true },
        { a: 7, b: 70, c: true },
        { a: 9, b: 90, c: true }
      ]);
      expectSearch('false', [
        { a: 0, b: 0, c: false },
        { a: 2, b: 20, c: false },
        { a: 4, b: 40, c: false },
        { a: 6, b: 60, c: false },
        { a: 8, b: 80, c: false }
      ]);
    });

    it('should test search keyword preparation', () => {
      const prepare = TableComponent.prepareSearch;
      const expected = ['a', 'b', 'c'];
      expect(prepare('a b c')).toEqual(expected);
      expect(prepare('a,, b,,  c')).toEqual(expected);
      expect(prepare('a,,,, b,,,     c')).toEqual(expected);
      expect(prepare('a+b c')).toEqual(['a+b', 'c']);
      expect(prepare('a,,,+++b,,,     c')).toEqual(['a+++b', 'c']);
      expect(prepare('"a b c"   "d e  f", "g, h i"')).toEqual(['a+b+c', 'd+e++f', 'g+h+i']);
    });

    it('should search for multiple values', () => {
      expectSearch('2 20 false', [{ a: 2, b: 20, c: false }]);
      expectSearch('false 2', [{ a: 2, b: 20, c: false }]);
    });

    it('should filter by column', () => {
      expectSearch('index:5', [{ a: 5, b: 50, c: true }]);
      expectSearch('times:50', [{ a: 5, b: 50, c: true }]);
      expectSearch('times:50 index:5', [{ a: 5, b: 50, c: true }]);
      expectSearch('Odd?:true', [
        { a: 1, b: 10, c: true },
        { a: 3, b: 30, c: true },
        { a: 5, b: 50, c: true },
        { a: 7, b: 70, c: true },
        { a: 9, b: 90, c: true }
      ]);
      component.data = createFakeData(100);
      expectSearch('index:1 odd:true times:110', [{ a: 11, b: 110, c: true }]);
    });

    it('should search through arrays', () => {
      component.columns = [{ prop: 'a', name: 'Index' }, { prop: 'b', name: 'ArrayColumn' }];

      component.data = [{ a: 1, b: ['foo', 'bar'] }, { a: 2, b: ['baz', 'bazinga'] }];
      expectSearch('bar', [{ a: 1, b: ['foo', 'bar'] }]);
      expectSearch('arraycolumn:bar arraycolumn:foo', [{ a: 1, b: ['foo', 'bar'] }]);
      expectSearch('arraycolumn:baz arraycolumn:inga', [{ a: 2, b: ['baz', 'bazinga'] }]);

      component.data = [{ a: 1, b: [1, 2] }, { a: 2, b: [3, 4] }];
      expectSearch('arraycolumn:1 arraycolumn:2', [{ a: 1, b: [1, 2] }]);
    });

    it('should search with spaces', () => {
      const expectedResult = [{ a: 2, b: 20, c: false }];
      expectSearch(`'Index times ten':20`, expectedResult);
      expectSearch('index+times+ten:20', expectedResult);
    });

    it('should filter results although column name is incomplete', () => {
      component.data = createFakeData(3);
      expectSearch(`'Index times ten'`, []);
      expectSearch(`'Ind'`, []);
      expectSearch(`'Ind:'`, [
        { a: 0, b: 0, c: false },
        { a: 1, b: 10, c: true },
        { a: 2, b: 20, c: false }
      ]);
    });

    it('should search if column name is incomplete', () => {
      const expectedData = [
        { a: 0, b: 0, c: false },
        { a: 1, b: 10, c: true },
        { a: 2, b: 20, c: false }
      ];
      component.data = _.clone(expectedData);
      expectSearch('inde', []);
      expectSearch('index:', expectedData);
      expectSearch('index times te', []);
    });

    it('should restore full table after search', () => {
      component.useData();
      expect(component.rows.length).toBe(10);
      component.search = '3';
      component.updateFilter();
      expect(component.rows.length).toBe(1);
      component.updateFilter(true);
      expect(component.rows.length).toBe(10);
    });
  });

  describe('after ngInit', () => {
    const toggleColumn = (prop, checked) => {
      component.toggleColumn({
        target: {
          name: prop,
          checked: checked
        }
      });
    };

    const equalStorageConfig = () => {
      expect(JSON.stringify(component.userConfig)).toBe(
        component.localStorage.getItem(component.tableName)
      );
    };

    beforeEach(() => {
      component.ngOnInit();
    });

    it('should have updated the column definitions', () => {
      expect(component.columns[0].flexGrow).toBe(1);
      expect(component.columns[1].flexGrow).toBe(2);
      expect(component.columns[2].flexGrow).toBe(2);
      expect(component.columns[2].resizeable).toBe(false);
    });

    it('should have table columns', () => {
      expect(component.tableColumns.length).toBe(3);
      expect(component.tableColumns).toEqual(component.columns);
    });

    it('should have a unique identifier which it searches for', () => {
      expect(component.identifier).toBe('a');
      expect(component.userConfig.sorts[0].prop).toBe('a');
      expect(component.userConfig.sorts).toEqual(component.createSortingDefinition('a'));
      equalStorageConfig();
    });

    it('should remove column "a"', () => {
      expect(component.userConfig.sorts[0].prop).toBe('a');
      toggleColumn('a', false);
      expect(component.userConfig.sorts[0].prop).toBe('b');
      expect(component.tableColumns.length).toBe(2);
      equalStorageConfig();
    });

    it('should not be able to remove all columns', () => {
      expect(component.userConfig.sorts[0].prop).toBe('a');
      toggleColumn('a', false);
      toggleColumn('b', false);
      toggleColumn('c', false);
      expect(component.userConfig.sorts[0].prop).toBe('c');
      expect(component.tableColumns.length).toBe(1);
      equalStorageConfig();
    });

    it('should enable column "a" again', () => {
      expect(component.userConfig.sorts[0].prop).toBe('a');
      toggleColumn('a', false);
      toggleColumn('a', true);
      expect(component.userConfig.sorts[0].prop).toBe('b');
      expect(component.tableColumns.length).toBe(3);
      equalStorageConfig();
    });

    afterEach(() => {
      clearLocalStorage();
    });
  });

  describe('reload data', () => {
    beforeEach(() => {
      component.ngOnInit();
      component.data = [];
      component['updating'] = false;
    });

    it('should call fetchData callback function', () => {
      component.fetchData.subscribe((context) => {
        expect(context instanceof CdTableFetchDataContext).toBeTruthy();
      });
      component.reloadData();
    });

    it('should call error function', () => {
      component.data = createFakeData(5);
      component.fetchData.subscribe((context) => {
        context.error();
        expect(component.loadingError).toBeTruthy();
        expect(component.data.length).toBe(0);
        expect(component.loadingIndicator).toBeFalsy();
        expect(component['updating']).toBeFalsy();
      });
      component.reloadData();
    });

    it('should call error function with custom config', () => {
      component.data = createFakeData(10);
      component.fetchData.subscribe((context) => {
        context.errorConfig.resetData = false;
        context.errorConfig.displayError = false;
        context.error();
        expect(component.loadingError).toBeFalsy();
        expect(component.data.length).toBe(10);
        expect(component.loadingIndicator).toBeFalsy();
        expect(component['updating']).toBeFalsy();
      });
      component.reloadData();
    });

    it('should update selection on refresh - "onChange"', () => {
      spyOn(component, 'onSelect').and.callThrough();
      component.data = createFakeData(10);
      component.selection.selected = [_.clone(component.data[1])];
      component.updateSelectionOnRefresh = 'onChange';
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalledTimes(0);
      component.data[1].d = !component.data[1].d;
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalled();
    });

    it('should update selection on refresh - "always"', () => {
      spyOn(component, 'onSelect').and.callThrough();
      component.data = createFakeData(10);
      component.selection.selected = [_.clone(component.data[1])];
      component.updateSelectionOnRefresh = 'always';
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalled();
      component.data[1].d = !component.data[1].d;
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalled();
    });

    it('should update selection on refresh - "never"', () => {
      spyOn(component, 'onSelect').and.callThrough();
      component.data = createFakeData(10);
      component.selection.selected = [_.clone(component.data[1])];
      component.updateSelectionOnRefresh = 'never';
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalledTimes(0);
      component.data[1].d = !component.data[1].d;
      component.updateSelected();
      expect(component.onSelect).toHaveBeenCalledTimes(0);
    });

    afterEach(() => {
      clearLocalStorage();
    });
  });

  describe('useCustomClass', () => {
    beforeEach(() => {
      component.customCss = {
        'label label-danger': 'active',
        'secret secret-number': 123.456,
        'btn btn-sm': (v) => _.isString(v) && v.startsWith('http'),
        secure: (v) => _.isString(v) && v.startsWith('https')
      };
    });

    it('should throw an error if custom classes are not set', () => {
      component.customCss = undefined;
      expect(() => component.useCustomClass('active')).toThrowError('Custom classes are not set!');
    });

    it('should not return any class', () => {
      ['', 'something', 123, { complex: 1 }, [1, 2, 3]].forEach((value) =>
        expect(component.useCustomClass(value)).toBe(undefined)
      );
    });

    it('should match a string and return the corresponding class', () => {
      expect(component.useCustomClass('active')).toBe('label label-danger');
    });

    it('should match a number and return the corresponding class', () => {
      expect(component.useCustomClass(123.456)).toBe('secret secret-number');
    });

    it('should match against a function and return the corresponding class', () => {
      expect(component.useCustomClass('http://no.ssl')).toBe('btn btn-sm');
    });

    it('should match against multiple functions and return the corresponding classes', () => {
      expect(component.useCustomClass('https://secure.it')).toBe('btn btn-sm secure');
    });
  });
});
