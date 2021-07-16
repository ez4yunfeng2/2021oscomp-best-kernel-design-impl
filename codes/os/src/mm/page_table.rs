use super::{
    frame_alloc,
    PhysPageNum,
    FrameTracker,
    VirtPageNum,
    VirtAddr,
    PhysAddr,
    StepByOne,
    Bytes
};
use crate::task::current_user_token;
use alloc::vec::Vec;
use alloc::vec;
use alloc::string::String;
use bitflags::*;

bitflags! {
    pub struct PTEFlags: u8 {
        const V = 1 << 0;
        const R = 1 << 1;
        const W = 1 << 2;
        const X = 1 << 3;
        const U = 1 << 4;
        const G = 1 << 5;
        const A = 1 << 6;
        const D = 1 << 7;
    }
}

#[derive(Copy, Clone)]
#[repr(C)]
pub struct PageTableEntry {
    pub bits: usize,
}

impl PageTableEntry {
    pub fn new(ppn: PhysPageNum, flags: PTEFlags) -> Self {
        PageTableEntry {
            bits: ppn.0 << 10 | flags.bits as usize,
        }
    }
    pub fn empty() -> Self {
        PageTableEntry {
            bits: 0,
        }
    }
    pub fn ppn(&self) -> PhysPageNum {
        (self.bits >> 10 & ((1usize << 44) - 1)).into()
    }
    pub fn flags(&self) -> PTEFlags {
        PTEFlags::from_bits(self.bits as u8).unwrap()
    }
    pub fn is_valid(&self) -> bool {
        (self.flags() & PTEFlags::V) != PTEFlags::empty()
    }
    pub fn readable(&self) -> bool {
        (self.flags() & PTEFlags::R) != PTEFlags::empty()
    }
    pub fn writable(&self) -> bool {
        (self.flags() & PTEFlags::W) != PTEFlags::empty()
    }
    pub fn executable(&self) -> bool {
        (self.flags() & PTEFlags::X) != PTEFlags::empty()
    }
    // only X+W+R can be set
    pub fn set_flags(&mut self, flags: usize) {
        self.bits = (self.bits & !(0b1110 as usize)) | ( flags & (0b1110 as usize));
    }
}


pub struct PageTable {
    root_ppn: PhysPageNum,
    frames: Vec<FrameTracker>,
}

/// Assume that it won't oom when creating/mapping.
impl PageTable {
    pub fn new() -> Self {
        let frame = frame_alloc().unwrap();
        PageTable {
            root_ppn: frame.ppn,
            frames: vec![frame],
        }
    }
    /// Temporarily used to get arguments from user space.
    pub fn from_token(satp: usize) -> Self {
        Self {
            root_ppn: PhysPageNum::from(satp & ((1usize << 44) - 1)),
            frames: Vec::new(),
        }
    }
    fn find_pte_create(&mut self, vpn: VirtPageNum) -> Option<&mut PageTableEntry> {
        let idxs = vpn.indexes();
        let mut ppn = self.root_ppn;
        let mut result: Option<&mut PageTableEntry> = None;
        for i in 0..3 {
            let pte = &mut ppn.get_pte_array()[idxs[i]];
            if i == 2 {
                result = Some(pte);
                break;
            }
            if !pte.is_valid() {
                let frame = frame_alloc().unwrap();
                *pte = PageTableEntry::new(frame.ppn, PTEFlags::V);
                self.frames.push(frame);
            }
            ppn = pte.ppn();
        }
        result
    }
    fn find_pte(&self, vpn: VirtPageNum) -> Option<&PageTableEntry> {
        let idxs = vpn.indexes();
        let mut ppn = self.root_ppn;
        let mut result: Option<&PageTableEntry> = None;
        for i in 0..3 {
            let pte = &ppn.get_pte_array()[idxs[i]];
            if i == 2 {
                result = Some(pte);
                break;
            }
            if !pte.is_valid() {
                return None;
            }
            ppn = pte.ppn();
        }
        result
    }
    
    // only X+W+R can be set
    pub fn set_pte_flags(&mut self, vpn: VirtPageNum, flags: usize) -> isize{
        let idxs = vpn.indexes();
        let mut ppn = self.root_ppn;
        let mut result: Option<&PageTableEntry> = None;
        for i in 0..3 {
            let pte = &mut ppn.get_pte_array()[idxs[i]];
            if i == 2 {
                // if pte == None{
                //     panic!("set_pte_flags: no such pte");
                // }
                // else{
                    pte.set_flags(flags);
                // }
                break;
            }
            if !pte.is_valid() {
                return -1;
            }
            ppn = pte.ppn();
        }
        0
    }
    
    #[allow(unused)]
    pub fn map(&mut self, vpn: VirtPageNum, ppn: PhysPageNum, flags: PTEFlags) {
        let pte = self.find_pte_create(vpn).unwrap();
        assert!(!pte.is_valid(), "vpn {:?} is mapped before mapping", vpn);
        *pte = PageTableEntry::new(ppn, flags | PTEFlags::V);
    }
    #[allow(unused)]
    pub fn unmap(&mut self, vpn: VirtPageNum) {
        let pte = self.find_pte_create(vpn).unwrap();
        assert!(pte.is_valid(), "vpn {:?} is invalid before unmapping", vpn);
        *pte = PageTableEntry::empty();
    }
    pub fn translate(&self, vpn: VirtPageNum) -> Option<PageTableEntry> {
        self.find_pte(vpn)
            .map(|pte| {pte.clone()})
    }
    pub fn translate_va(&self, va: VirtAddr) -> Option<PhysAddr> {
        self.find_pte(va.clone().floor())
            .map(|pte| {
                let aligned_pa: PhysAddr = pte.ppn().into();
                let offset = va.page_offset();
                let aligned_pa_usize: usize = aligned_pa.into();
                (aligned_pa_usize + offset).into()
            })
    }
    pub fn token(&self) -> usize {
        8usize << 60 | self.root_ppn.0
    }
}

pub fn translated_byte_buffer(token: usize, ptr: *const u8, len: usize) -> Vec<&'static mut [u8]> {
    let page_table = PageTable::from_token(token);
    let mut start = ptr as usize;
    let end = start + len;
    let mut v = Vec::new();
    while start < end {
        let start_va = VirtAddr::from(start);
        let mut vpn = start_va.floor();
        let ppn = page_table
            .translate(vpn)
            .unwrap()
            .ppn();
        vpn.step();
        let mut end_va: VirtAddr = vpn.into();
        end_va = end_va.min(VirtAddr::from(end));
        if end_va.page_offset() == 0 {
            v.push(&mut ppn.get_bytes_array()[start_va.page_offset()..]);
        } else {
            v.push(&mut ppn.get_bytes_array()[start_va.page_offset()..end_va.page_offset()]);
        }
        start = end_va.into();
    }
    v
}

/// Load a string from other address spaces into kernel space without an end `\0`.
pub fn translated_str(token: usize, ptr: *const u8) -> String {
    let page_table = PageTable::from_token(token);
    let mut string = String::new();
    let mut va = ptr as usize;
    loop {
        let ch: u8 = *(page_table.translate_va(VirtAddr::from(va)).unwrap().get_mut());
        if ch == 0 {
            break;
        }
        string.push(ch as char);
        va += 1;
    }
    string
}

pub fn translated_ref<T>(token: usize, ptr: *const T) -> &'static T {
    let page_table = PageTable::from_token(token);
    page_table.translate_va(VirtAddr::from(ptr as usize)).unwrap().get_ref()
}

pub fn translated_refmut<T>(token: usize, ptr: *mut T) -> &'static mut T {
    let page_table = PageTable::from_token(token);
    let va = ptr as usize;
    page_table.translate_va(VirtAddr::from(va)).unwrap().get_mut()
}

/* 获取用户数组内各元素的引用 */
/* 目前并不能处理跨页结构体 */
pub fn translated_ref_array<T>(token: usize, ptr: *mut T, len: usize) -> Vec<&'static T>{
    let page_table = PageTable::from_token(token);
    let mut ref_array:Vec<&'static T> = Vec::new();
    let mut va = ptr as usize;
    let step = core::mem::size_of::<T>();
    for i in 0..len {
        println!("[trans array] va = 0x{:X}", va);
        ref_array.push( page_table.translate_va(VirtAddr::from(va)).unwrap().get_ref() );
        va += step;
    }
    ref_array
}

/* 获取用户数组的一份拷贝 */
pub fn translated_array_copy<T>(token: usize, ptr: *mut T, len: usize) -> Vec< T>
    where T:Copy {
    let page_table = PageTable::from_token(token);
    let mut ref_array:Vec<T> = Vec::new();
    let mut va = ptr as usize;
    let step = core::mem::size_of::<T>();
    //println!("step = {}, len = {}", step, len);
    for _i in 0..len {
        let u_buf = UserBuffer::new( translated_byte_buffer(token, va as *const u8, step) );
        let mut bytes_vec:Vec<u8> = Vec::new();
        u_buf.read_as_vec(&mut bytes_vec, step);
        //println!("loop, va = 0x{:X}, vec = {:?}", va, bytes_vec);
        unsafe{
            ref_array.push(  *(bytes_vec.as_slice() as *const [u8] as *const u8 as usize as *const T) );
        }
        va += step;
    }
    ref_array
}


fn trans_to_bytes<T>(ptr: *const T)->&'static[u8]{
    let size = core::mem::size_of::<T>();
    unsafe {
        core::slice::from_raw_parts(
            ptr as usize as *const u8,
            size,
        )
    }
}

fn trans_to_bytes_mut<T>(ptr: *mut T)->&'static mut [u8]{
    let size = core::mem::size_of::<T>();
    unsafe {
        core::slice::from_raw_parts_mut(
            ptr as usize as *mut u8,
            size,
        )
    }
}


/* 从用户空间复制数据到指定地址 */
pub fn copy_from_user<T>(dst: *mut T, src: usize, size: usize) {
    let token = current_user_token();
    // translated_ 实际上完成了地址合法检测
    let buf = UserBuffer::new(translated_byte_buffer(token, src as *const u8, size));
    let mut dst_bytes = trans_to_bytes_mut(dst);
    buf.read(dst_bytes);
}

/* 从指定地址复制数据到用户空间 */
pub fn copy_to_user<T>(dst: usize, src: *const T, size: usize) {
    let token = current_user_token();
    let mut buf = UserBuffer::new(translated_byte_buffer(token, dst as *const u8, size));
    let src_bytes = trans_to_bytes(src);
    buf.write(src_bytes);
}


pub struct UserBuffer {
    pub buffers: Vec<&'static mut [u8]>,
}

impl UserBuffer {
    pub fn new(buffers: Vec<&'static mut [u8]>) -> Self {
        Self { buffers }
    }

    pub fn len(&self) -> usize {
        let mut total: usize = 0;
        for b in self.buffers.iter() {
            total += b.len();
        }
        total
    }

    // 将一个Buffer的数据写入UserBuffer，返回写入长度
    pub fn write(&mut self, buff: &[u8])->usize{
        let len = self.len().min(buff.len());
        let mut current = 0;
        for sub_buff in self.buffers.iter_mut() {
            let sblen = (*sub_buff).len();
            for j in 0..sblen {
                (*sub_buff)[j] = buff[current];
                current += 1;
                if current == len {
                    return len;
                }
            }
        }
        return len;
    }

    // 将UserBuffer的数据读入一个Buffer，返回读取长度
    pub fn read(&self, buff:&mut [u8])->usize{
        let len = self.len().min(buff.len());
        let mut current = 0;
        for sub_buff in self.buffers.iter() {
            let sblen = (*sub_buff).len();
            for j in 0..sblen {
                buff[current] = (*sub_buff)[j];
                current += 1;
                if current == len {
                    return len;
                }
            }
        }
        return len;
    }

    // TODO: 把vlen去掉    
    pub fn read_as_vec(&self, vec: &mut Vec<u8>, vlen:usize)->usize{
        let len = self.len();
        let mut current = 0;
        for sub_buff in self.buffers.iter() {
            let sblen = (*sub_buff).len();
            for j in 0..sblen {
                vec.push( (*sub_buff)[j] );
                current += 1;
                if current == len {
                    return len;
                }
            }
        }
        return len;
    }

   
}

impl IntoIterator for UserBuffer {
    type Item = *mut u8;
    type IntoIter = UserBufferIterator;
    fn into_iter(self) -> Self::IntoIter {
        UserBufferIterator {
            buffers: self.buffers,
            current_buffer: 0,
            current_idx: 0,
        }
    }
}

pub struct UserBufferIterator {
    buffers: Vec<&'static mut [u8]>,
    current_buffer: usize,
    current_idx: usize,
}

impl Iterator for UserBufferIterator {
    type Item = *mut u8;
    fn next(&mut self) -> Option<Self::Item> {
        if self.current_buffer >= self.buffers.len() {
            None
        } else {
            let r = &mut self.buffers[self.current_buffer][self.current_idx] as *mut _;
            if self.current_idx + 1 == self.buffers[self.current_buffer].len() {
                self.current_idx = 0;
                self.current_buffer += 1;
            } else {
                self.current_idx += 1;
            }
            Some(r)
        }
    }
}