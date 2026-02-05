from fastapi import FastAPI, File, UploadFile, HTTPException, Form # 导入FastAPI和相关的请求和响应类
from fastapi.responses import JSONResponse, FileResponse # 导入JSONResponse和FileResponse类
import os # 导入os模块，用于文件操作
from datetime import datetime # 导入datetime模块，用于生成唯一文件名
import json # 导入json模块，用于处理JSON数据
import glob # 导入glob模块，用于文件路径匹配
from typing import List # 导入List类型提示

# 导入封装好的功能类
from calibration import CameraCalibrator
from image_process import ImageProcessor

app = FastAPI(title="相机标定与图像处理系统")

# 目录配置
UPLOAD_FOLDER = "temp_images"
CALIB_IMAGES_FOLDER = "calib_images"
PROCESSED_FOLDER = "processed_images"
CALIB_RESULTS_FOLDER = "calib_results"
RESULTS_FOLDER = "results"

# 创建必要的目录
for folder in [UPLOAD_FOLDER, CALIB_IMAGES_FOLDER, PROCESSED_FOLDER, CALIB_RESULTS_FOLDER, RESULTS_FOLDER]:
    os.makedirs(folder, exist_ok=True)

# 全局变量存储当前标定参数
calibration_params = {
    "cameraMatrix": None,
    "distCoeffs": None,
    "newCameraMatrix": None,
    "roi": None,
    "is_calibrated": False
}

# -----------------------------------------------------------------------
# -------------------------- 上传标定图片接口 ----------------------------
# 请求示例：curl -X POST "http://{PUBLIC_IP}:{PORT}/upload_calib_image" -F "files=@本地文件路径" -F "files=@本地文件路径"
# 响应示例：{"status": "success", "message": "标定图片上传成功", "filename": "20240101120000_image.jpg", "file_path": "calib_images/20240101120000_image.jpg"}
# -----------------------------------------------------------------------
@app.post("/upload_calib_image")
async def upload_calib_image(files: List[UploadFile] = File(...)):
    try:
        # 存储所有文件的上传结果
        upload_results = []
        
        # 遍历每个上传的文件
        for file in files:
            # 生成唯一文件名
            timestamp = datetime.now().strftime('%Y%m%d%H%M%S')
            # 为避免文件名重复，在时间戳后添加随机数或使用文件对象ID的一部分
            filename = f"{timestamp}_{str(id(file))[:6]}_{file.filename}"
            file_path = os.path.join(CALIB_IMAGES_FOLDER, filename)

            # 保存图片
            with open(file_path, "wb") as f:
                f.write(await file.read())
                
            # 记录单个文件的上传结果
            upload_results.append({
                "status": "success",
                "filename": filename,
                "original_filename": file.filename,
                "file_path": file_path
            })

        return JSONResponse(
            content={
                "status": "success",
                "message": f"成功上传{len(upload_results)}张标定图片",
                "uploaded_files": upload_results
            }
        )
    except Exception as e:
        return JSONResponse(status_code=500, content={"error": str(e)})

# -----------------------------------------------------------------------
# -------------------------- 开始标定接口 --------------------------------
# 请求示例：curl -X POST "http://{PUBLIC_IP}:{PORT}/start_calibration" -F "chessboard_size=6,6" -F "square_size=35.0"
# 响应示例：{"status": "success", "message": "相机标定成功", "results": {...}}
# -----------------------------------------------------------------------
@app.post("/start_calibration")
async def start_calibration(
    chessboard_size: str = Form("6,6"),  # 格式: "width,height"
    square_size: float = Form(35.0)     # 单位: mm
):
    try:
        # 解析棋盘格尺寸
        cb_width, cb_height = map(int, chessboard_size.split(","))
        CHESSBOARD_SIZE = (cb_width, cb_height)
        SQUARE_SIZE = square_size

        # 批量读取标定图
        calib_images = glob.glob(os.path.join(CALIB_IMAGES_FOLDER, "*.jpg")) + \
                       glob.glob(os.path.join(CALIB_IMAGES_FOLDER, "*.png"))
        
        if len(calib_images) == 0:
            raise HTTPException(status_code=400, detail="未找到标定图片，请先上传标定图片")

        # 创建标定器实例并执行标定
        calibrator = CameraCalibrator(chessboard_size=CHESSBOARD_SIZE, square_size=SQUARE_SIZE)
        results = calibrator.calibrate(img_paths=calib_images)
        
        # 保存标定结果
        calibrator.save_results()

        # 更新全局标定参数
        calibration_params.update({
            "cameraMatrix": results["cameraMatrix"],
            "distCoeffs": results["distCoeffs"],
            "newCameraMatrix": results["newCameraMatrix"],
            "roi": results["roi"],
            "is_calibrated": True
        })

        return JSONResponse(
            content={
                "status": "success",
                "message": "相机标定成功",
                "results": {
                    "valid_images_count": results["valid_images_count"],
                    "total_images_count": results["total_images_count"],
                    "mean_reproj_error": results["mean_reproj_error"],
                    "camera_matrix": results["cameraMatrix"].tolist(),
                    "dist_coeffs": results["distCoeffs"].tolist()
                }
            }
        )
    except Exception as e:
        if isinstance(e, HTTPException):
            raise e
        return JSONResponse(status_code=500, content={"error": str(e)})

# --------------------------------------------------------------------------
# -------------------------- 开始图像处理接口 --------------------------------
# 请求示例：curl -X POST "http://{PUBLIC_IP}:{PORT}/process_image" -F "file=@本地文件路径"
# 响应示例：{"status": "success", "message": "图像处理成功", "filename": "20240101120000_processed.jpg", "file_path": "processed/20240101120000_processed.jpg"}
# --------------------------------------------------------------------------
@app.post("/process_image")
async def process_image(file: UploadFile = File(...)):
    try:
        # 检查是否已标定
        if not calibration_params["is_calibrated"]:
            # 尝试从文件加载标定参数
            calib_params_path = os.path.join(CALIB_RESULTS_FOLDER, "camera_calib_params.xml")
            if not os.path.exists(calib_params_path):
                raise HTTPException(status_code=400, detail="请先进行相机标定")
            
            # 创建标定器实例并加载参数
            calibrator = CameraCalibrator()
            try:
                calibrator.load_results(calib_params_path)
                # 更新全局参数
                calibration_params.update({
                    "cameraMatrix": calibrator.cameraMatrix,
                    "distCoeffs": calibrator.distCoeffs,
                    "newCameraMatrix": calibrator.newCameraMatrix,
                    "roi": calibrator.roi,
                    "is_calibrated": True
                })
            except Exception as e:
                raise HTTPException(status_code=400, detail=f"标定参数加载失败: {str(e)}")

        # 生成唯一文件名
        timestamp = datetime.now().strftime('%Y%m%d%H%M%S')
        original_filename = f"{timestamp}_original.jpg"
        processed_filename = f"{timestamp}_processed.jpg"
        result_filename = f"{timestamp}_result.json"
        
        original_path = os.path.join(UPLOAD_FOLDER, original_filename)
        processed_path = os.path.join(PROCESSED_FOLDER, processed_filename)
        result_path = os.path.join(RESULTS_FOLDER, result_filename)

        # 保存原图片
        with open(original_path, "wb") as f:
            f.write(await file.read())

        # 创建图像处理器实例并处理图像
        processor = ImageProcessor(os.path.join(CALIB_RESULTS_FOLDER, "camera_calib_params.xml"))
        process_results = processor.process_image(original_path)

        # 保存处理后的图像
        import cv2
        cv2.imwrite(processed_path, process_results["result_image"])

        # 保存检测结果数据
        with open(result_path, 'w') as f:
            json.dump({
                "timestamp": timestamp,
                "scale": process_results["scale"],
                "detected_rectangles": process_results["detected_rectangles"]
            }, f, indent=2)

        # 清理原临时图片
        os.remove(original_path)

        # 返回结果
        return JSONResponse(
            content={
                "status": "success",
                "result": {
                    "scale": process_results["scale"],
                    "detected_rectangles": process_results["detected_rectangles"],
                    "processed_image_filename": processed_filename,
                    "result_data_filename": result_filename,
                    "download_image_url": f"/download/image/{processed_filename}",
                    "download_data_url": f"/download/data/{result_filename}"
                }
            }
        )
    except Exception as e:
        if isinstance(e, HTTPException):
            raise e
        return JSONResponse(status_code=500, content={"error": str(e)})

# ----------------------------------------------------------------------------------
# -------------------------- 下载图像处理后的结果接口 --------------------------------
# 请求示例：curl -X GET "http://{PUBLIC_IP}:{PORT}/download/image/20240101120000_processed.jpg"
# 响应示例：返回处理后的图像文件
# ----------------------------------------------------------------------------------
@app.get("/download/image/{filename}")
async def download_processed_image(filename: str):
    file_path = os.path.join(PROCESSED_FOLDER, filename)
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="文件不存在")
    return FileResponse(
        path=file_path,
        filename=filename,
        media_type="image/jpeg"
    )

@app.get("/download/data/{filename}")
async def download_result_data(filename: str):
    file_path = os.path.join(RESULTS_FOLDER, filename)
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="文件不存在")
    return FileResponse(
        path=file_path,
        filename=filename,
        media_type="application/json"
    )

# ----------------------------------------------------------------------------------
# -------------------------- 下载标定文件接口 ----------------------------------------
# 请求示例：curl -X GET "http://{PUBLIC_IP}:{PORT}/download/calib/camera_calib_params.xml"
# 响应示例：返回标定参数文件
# ----------------------------------------------------------------------------------
@app.get("/download/calib/{filename}")
async def download_calib_file(filename: str):
    file_path = os.path.join(CALIB_RESULTS_FOLDER, filename)
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="文件不存在")
    return FileResponse(
        path=file_path,
        filename=filename,
        media_type="application/octet-stream"
    )

# ----------------------------------------------------------------------------------
# -------------------------- 查看标定状态接口 ----------------------------------------
# 请求示例：curl -X GET "http://{PUBLIC_IP}:{PORT}/calibration/status"
# 响应示例：{"is_calibrated": true, "has_calib_params_file": true}
# ----------------------------------------------------------------------------------
@app.get("/calibration/status")
async def get_calibration_status():
    return JSONResponse(
        content={
            "is_calibrated": calibration_params["is_calibrated"],
            "has_calib_params_file": os.path.exists(os.path.join(CALIB_RESULTS_FOLDER, "camera_calib_params.xml"))
        }
    )

# ----------------------------------------------------------------------------------
# -------------------------- 删除标定图片接口 ----------------------------------------
# 请求示例：curl -X DELETE "http://{PUBLIC_IP}:{PORT}/delete_calib_images"
# 响应示例：{"status": "success", "message": "成功删除5张标定图片", "deleted_files": ["file1.jpg", "file2.jpg", ...]}
# ----------------------------------------------------------------------------------
@app.delete("/delete_calib_images")
async def delete_calib_images():
    try:
        # 获取标定图片文件夹中的所有文件
        calib_images = glob.glob(os.path.join(CALIB_IMAGES_FOLDER, "*"))
        deleted_files = []
        
        # 遍历并删除每个文件
        for file_path in calib_images:
            if os.path.isfile(file_path):
                os.remove(file_path)
                deleted_files.append(os.path.basename(file_path))
        
        return JSONResponse(
            content={
                "status": "success",
                "message": f"成功删除{len(deleted_files)}张标定图片",
                "deleted_files": deleted_files
            }
        )
    except Exception as e:
        return JSONResponse(status_code=500, content={"error": str(e)})

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=5001)